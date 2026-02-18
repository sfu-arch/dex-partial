/**
 * tbc_bench.cpp — Benchmark for Trie+Bitmap Cache B+-Tree
 *
 * Modelled after DEX's test/newbench.cpp so that results are directly
 * comparable.  Uses the same:
 *   - DSM setup (RDMA cluster via memcached)
 *   - Workload encoding (op_type in top 8 bits of uint64_t)
 *   - CityHash key derivation
 *   - Zipfian / uniform distribution
 *   - Throughput collection (ops/s per thread, aggregated)
 *
 * The ONLY difference: instead of cachepush::BTree (DEX), we create a
 * tbc::TrieBitmapTree — same tree_api<Key,Value>* polymorphism.
 */

#include "Timer.h"
#include "TrieBitmapTree.h"
#include "zipf.h"
#include "uniform.h"
#include "uniform_generator.h"

#include <algorithm>
#include <city.h>
#include <cmath>
#include <iostream>
#include <mutex>
#include <numa.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// ---- Workload encoding (same as DEX newbench.cpp) --------------------
enum op_type : uint8_t { Insert = 0, Update, Lookup, Delete, Range };
static constexpr uint64_t op_mask = (1ULL << 56) - 1;

// ---- Global state ----------------------------------------------------
int kMaxThread = 32;
std::thread th[MAX_APP_THREAD];
uint64_t tp[MAX_APP_THREAD][8];
uint64_t total_tp[MAX_APP_THREAD];

uint32_t kReadRatio, kInsertRatio, kUpdateRatio, kDeleteRatio, kRangeRatio;
int kThreadCount, totalThreadCount, memThreadCount, kNodeCount, CNodeCount;
uint64_t cache_mb, kKeySpace, threadKSpace;
uint64_t op_num, thread_op_num, thread_warmup_num;
uint64_t bulk_load_num, warmup_num;
int node_id = 0;
double zipfian = 0.99;
int uniform_workload = 0;
int time_based = 1;
int early_stop = 1;

uint64_t *bulk_array    = nullptr;
uint64_t *workload_array = nullptr;
uint64_t *warmup_array   = nullptr;

struct zipf_gen_state state;
uniform_key_generator_t *uniform_generator = nullptr;

tree_api<Key, Value> *tree;
DSM *dsm;

std::atomic<int64_t>  warmup_cnt{0};
std::atomic<uint64_t> worker{0};
std::atomic<uint64_t> execute_op{0};
std::atomic_bool      ready{false};
std::atomic_bool      one_finish{false};
std::atomic_bool      ready_to_report{false};

// ---- Key generation (identical to DEX) --------------------------------
inline Key to_key(uint64_t k) {
    return (CityHash64(reinterpret_cast<char*>(&k), sizeof(k)) + 1) % kKeySpace;
}

uint64_t generate_range_key() {
    uint64_t key = 0;
    while (true) {
        if (uniform_workload) {
            uint64_t dis = uniform_generator->next_id();
            key = to_key(dis);
        } else {
            uint64_t dis = mehcached_zipf_next(&state);
            key = to_key(dis);
        }
        break;
    }
    return key;
}

// ---- Worker thread (same pattern as DEX) ------------------------------
void thread_run(int id) {
    bindCore(id);
    dsm->registerThread();
    tp[id][0] = 0;
    total_tp[id] = 0;
    worker.fetch_add(1);

    uint64_t *tw = warmup_array  + id * thread_warmup_num;
    uint64_t *to = workload_array + id * thread_op_num;
    uint32_t scan_num = 100;
    auto *result = new std::pair<Key, Value>[scan_num];

    // ---- Warmup phase ----
    size_t counter = 0;
    while (counter < thread_warmup_num) {
        uint64_t key = tw[counter];
        op_type cur_op = static_cast<op_type>(key >> 56);
        key &= op_mask;
        switch (cur_op) {
        case Lookup: { Value v; tree->lookup(key, v); } break;
        case Insert: { tree->insert(key, key + 1);    } break;
        case Update: { tree->update(key, key);         } break;
        case Delete: { tree->remove(key);              } break;
        case Range:  { tree->range_scan(key, scan_num, result); } break;
        }
        ++counter;
    }

    warmup_cnt.fetch_add(1);
    if (id == 0) {
        while (warmup_cnt.load() != kThreadCount) ;
        printf("[TBC] Node %d warmup done\n", dsm->getMyNodeID());
        dsm->clear_rdma_statistic();
        tree->clear_statistic();
        dsm->barrier("tbc_warm_finish", CNodeCount);
        ready.store(true);
    }
    while (!ready_to_report.load()) ;

    // ---- Main workload ----
    counter = 0;
    auto start = std::chrono::high_resolution_clock::now();
    while (counter < thread_op_num) {
        uint64_t key = to[counter];
        op_type cur_op = static_cast<op_type>(key >> 56);
        key &= op_mask;
        switch (cur_op) {
        case Lookup: { Value v; tree->lookup(key, v); } break;
        case Insert: { tree->insert(key, key + 1);    } break;
        case Update: { tree->update(key, key);         } break;
        case Delete: { tree->remove(key);              } break;
        case Range:  { tree->range_scan(key, scan_num, result); } break;
        }
        ++tp[id][0];
        ++counter;
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto us  = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    if (early_stop && !one_finish.load()) {
        one_finish.store(true);
        thread_op_num = 0;
    }
    worker.fetch_sub(1);
    total_tp[id] = static_cast<uint64_t>(counter / (us / 1e6));
    execute_op.fetch_add(counter);
    delete[] result;
}

// ---- Argument parsing (same 23-arg format as DEX) ---------------------
void parse_args(int argc, char *argv[]) {
    if (argc < 23) {
        printf("Usage: ./tbc_bench  kNodeCount kReadRatio kInsertRatio "
               "kUpdateRatio kDeleteRatio kRangeRatio totalThreadCount "
               "memThreadCount cacheSize(MB) uniform_workload zipfian_theta "
               "bulk_load_num(M) warmup_num(M) op_num(M) "
               "check_correctness time_based early_stop index "
               "rpc_rate admission_rate auto_tune kMaxThread\n");
        exit(-1);
    }
    kNodeCount       = atoi(argv[1]);
    kReadRatio       = atoi(argv[2]);
    kInsertRatio     = atoi(argv[3]);
    kUpdateRatio     = atoi(argv[4]);
    kDeleteRatio     = atoi(argv[5]);
    kRangeRatio      = atoi(argv[6]);
    totalThreadCount = atoi(argv[7]);
    memThreadCount   = atoi(argv[8]);
    cache_mb         = atoi(argv[9]);
    uniform_workload = atoi(argv[10]);
    zipfian          = atof(argv[11]);
    bulk_load_num    = atoi(argv[12]) * 1000000ULL;
    warmup_num       = atoi(argv[13]) * 1000000ULL;
    op_num           = atoi(argv[14]) * 1000000ULL;
    time_based       = atoi(argv[16]);
    early_stop       = atoi(argv[17]);
    kMaxThread       = atoi(argv[22]);

    kKeySpace = bulk_load_num +
                static_cast<uint64_t>(
                    std::ceil((op_num + warmup_num) * (kInsertRatio / 100.0)))
                + 1000;
    threadKSpace = kKeySpace / totalThreadCount;
    CNodeCount   = (totalThreadCount + kMaxThread - 1) / kMaxThread;
    thread_op_num     = op_num     / totalThreadCount;
    thread_warmup_num = warmup_num / totalThreadCount;

    printf("[TBC] kNodeCount=%d  threads=%d  cache=%luMB  zipf=%.2f  "
           "bulk=%luM  ops=%luM  R/I/U/D/S=%u/%u/%u/%u/%u\n",
           kNodeCount, totalThreadCount, cache_mb, zipfian,
           bulk_load_num / 1000000, op_num / 1000000,
           kReadRatio, kInsertRatio, kUpdateRatio, kDeleteRatio, kRangeRatio);
}

// ---- Workload generation (same encoding as DEX) -----------------------
void generate_workload() {
    auto *space = new uint64_t[kKeySpace];
    for (uint64_t i = 0; i < kKeySpace; ++i) space[i] = i;

    std::mt19937 gen(0xc70f6907UL);
    std::shuffle(space, space + kKeySpace, gen);

    bulk_array = new uint64_t[bulk_load_num];
    memcpy(bulk_array, space, sizeof(uint64_t) * bulk_load_num);

    uint64_t node_warmup = thread_warmup_num * kThreadCount;
    uint64_t node_ops    = thread_op_num     * kThreadCount;

    if (uniform_workload)
        uniform_generator = new uniform_key_generator_t(kKeySpace);
    else
        mehcached_zipf_init(&state, kKeySpace, zipfian,
                            (rdtsc() & 0x0000ffffffffffffULL) ^ node_id);

    UniformRandom rng(rdtsc() ^ node_id);
    uint32_t ins_mark = kReadRatio + kInsertRatio;
    uint32_t upd_mark = ins_mark   + kUpdateRatio;
    uint32_t del_mark = upd_mark   + kDeleteRatio;

    // Warmup array
    warmup_array = new uint64_t[node_warmup];
    for (uint64_t i = 0; i < node_warmup; ++i) {
        uint32_t r = rng.next_uint32() % 100;
        uint64_t key = generate_range_key();
        if (r < kReadRatio)         key |= (uint64_t(Lookup) << 56);
        else if (r < ins_mark)      key |= (uint64_t(Insert) << 56);
        else if (r < upd_mark)      key |= (uint64_t(Update) << 56);
        else if (r < del_mark)      key |= (uint64_t(Delete) << 56);
        else                        key |= (uint64_t(Range)  << 56);
        warmup_array[i] = key;
    }

    // Main workload
    workload_array = new uint64_t[node_ops];
    for (uint64_t i = 0; i < node_ops; ++i) {
        uint32_t r = rng.next_uint32() % 100;
        uint64_t key = generate_range_key();
        if (r < kReadRatio)         key |= (uint64_t(Lookup) << 56);
        else if (r < ins_mark)      key |= (uint64_t(Insert) << 56);
        else if (r < upd_mark)      key |= (uint64_t(Update) << 56);
        else if (r < del_mark)      key |= (uint64_t(Delete) << 56);
        else                        key |= (uint64_t(Range)  << 56);
        workload_array[i] = key;
    }

    delete[] space;
    printf("[TBC] Workload generated: warmup=%lu  main=%lu\n",
           node_warmup, node_ops);
}

// ---- Main -------------------------------------------------------------
int main(int argc, char *argv[]) {
    bindCore(0);
    numa_set_preferred(0);
    parse_args(argc, argv);

    // DSM setup (identical to DEX)
    DSMConfig config;
    config.machineNR    = kNodeCount;
    config.memThreadCount = memThreadCount;
    config.computeNR    = CNodeCount;
    config.index_type   = 0; // not relevant for TBC
    dsm = DSM::getInstance(config);

    node_id = dsm->getMyNodeID();
    kThreadCount = (node_id == CNodeCount - 1)
                       ? totalThreadCount - (CNodeCount - 1) * kMaxThread
                       : kMaxThread;

    if (node_id < CNodeCount) {
        dsm->registerThread();

        // Create TBC tree (instead of cachepush::BTree)
        tree = new tbc::TrieBitmapTree(dsm, 0, cache_mb);

        dsm->barrier("tbc_bulkload", CNodeCount);
        dsm->resetThread();

        generate_workload();

        // Bulk load
        tree->bulk_load(bulk_array, bulk_load_num);
        delete[] bulk_array;
        printf("[TBC] Node %d bulk load done\n", node_id);

        // Reset for benchmark
        thread_op_num     = op_num / totalThreadCount;
        thread_warmup_num = warmup_num / totalThreadCount;
        dsm->resetThread();
        dsm->registerThread();
        tree->reset_buffer_pool(true);
        dsm->barrier("tbc_benchmark", CNodeCount);
        tree->get_newest_root();

        dsm->resetThread();
        warmup_cnt.store(0);
        worker.store(0);
        ready.store(false);
        one_finish.store(false);
        ready_to_report.store(false);

        // Spawn worker threads
        for (int i = 0; i < kThreadCount; ++i)
            th[i] = std::thread(thread_run, i);

        // Wait for warmup
        while (!ready.load()) sleep(1);
        ready_to_report.store(true);

        // Collect throughput every 2 seconds
        timespec s, e;
        clock_gettime(CLOCK_REALTIME, &s);
        uint64_t pre_tp = 0;
        auto t_start = std::chrono::high_resolution_clock::now();

        while (worker.load() > 0) {
            sleep(2);
            clock_gettime(CLOCK_REALTIME, &e);
            int us = (e.tv_sec - s.tv_sec) * 1000000 +
                     (e.tv_nsec - s.tv_nsec) / 1000;

            uint64_t cur_tp = 0;
            for (int i = 0; i < kThreadCount; ++i)
                cur_tp += tp[i][0];

            uint64_t delta = cur_tp - pre_tp;
            printf("[TBC] Throughput: %.2f Mops/s  (interval %.2f Mops/s)\n",
                   cur_tp / (us / 1e6) / 1e6,
                   delta / 2.0 / 1e6);
            pre_tp = cur_tp;

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::high_resolution_clock::now() - t_start).count();
            if (time_based && elapsed >= 60) {
                thread_op_num = 0;  // signal threads to stop
            }
        }

        for (int i = 0; i < kThreadCount; ++i) th[i].join();

        // Final report
        uint64_t sum_tp = 0;
        for (int i = 0; i < kThreadCount; ++i)
            sum_tp += total_tp[i];
        printf("[TBC] === FINAL: %lu ops/s (%.2f Mops/s) ===\n",
               sum_tp, sum_tp / 1e6);

        tree->get_statistic();
    }

    printf("[TBC] Done.\n");
    return 0;
}
