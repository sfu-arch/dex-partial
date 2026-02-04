/**
 * CHIME Microbenchmark with Latency Tracking
 * 
 * This is a complete microbenchmark that matches DEX's newbench_latency.cpp
 * - No YCSB files needed
 * - Direct key generation (Zipfian or Uniform)
 * - Bulk loading
 * - Configurable read/write/update/scan ratios
 * - Detailed latency histogram capture
 * 
 * Usage: ./microbench_latency <args...>
 * Same parameters as DEX newbench_latency for easy comparison
 */

#include "Tree.h"
#include "Timer.h"
#include "Key.h"
#include <city.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdlib.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

// Configuration
#define TEST_EPOCH 10
#define TIME_INTERVAL 1.0
#define LATENCY_BUCKETS 100000    // 0-50ms with 500ns granularity
#define LATENCY_NS_GRANULARITY 500  // 500 nanoseconds per bucket

// External statistics
extern double cache_miss[MAX_APP_THREAD];
extern double cache_hit[MAX_APP_THREAD];
extern uint64_t lock_fail[MAX_APP_THREAD];
extern uint64_t write_handover_num[MAX_APP_THREAD];
extern uint64_t try_write_op[MAX_APP_THREAD];
extern uint64_t read_handover_num[MAX_APP_THREAD];
extern uint64_t try_read_op[MAX_APP_THREAD];
extern uint64_t read_leaf_retry[MAX_APP_THREAD];
extern uint64_t try_read_leaf[MAX_APP_THREAD];
extern uint64_t try_insert_op[MAX_APP_THREAD];
extern uint64_t split_node[MAX_APP_THREAD];
extern uint64_t retry_cnt[MAX_APP_THREAD][MAX_FLAG_NUM];

extern volatile bool need_stop;
extern uint64_t latency[MAX_APP_THREAD][MAX_CORO_NUM][LATENCY_WINDOWS];

// ============================================
// Global Configuration (matches DEX)
// ============================================
int kNodeCount;
int kThreadCount;
int kMaxThread = 16;
uint32_t kReadRatio;
uint32_t kInsertRatio;
uint32_t kUpdateRatio;
uint32_t kDeleteRatio;
uint32_t kRangeRatio;
int memThreadCount;
uint64_t cache_mb;
int uniform_workload = 0;
double zipfian = 0.99;
uint64_t bulk_load_num = 0;
uint64_t warmup_num = 0;
uint64_t op_num = 0;
int check_correctness = 0;
int time_based = 1;
int early_stop = 0;
int tree_index = 0;  // Not used in CHIME, but kept for compatibility
double rpc_rate = 1.0;
double admission_rate = 1.0;
int auto_tune = 0;

// Derived values
uint64_t kKeySpace;
uint64_t thread_op_num;
uint64_t thread_warmup_num;
int node_id = 0;
int CNodeCount;

// Thread management
std::thread th[MAX_APP_THREAD];
uint64_t tp[MAX_APP_THREAD][8];
uint64_t total_tp[MAX_APP_THREAD];

// Latency histogram - per thread to avoid contention
uint64_t latency_histogram[MAX_APP_THREAD][LATENCY_BUCKETS];
std::atomic<uint64_t> total_latency_samples{0};

// Synchronization
std::atomic<int64_t> warmup_cnt{0};
std::atomic<uint64_t> worker{0};
std::atomic<uint64_t> execute_op{0};
std::atomic_bool ready{false};
std::atomic_bool ready_to_report{false};

// Workload arrays
uint64_t *bulk_array = nullptr;
uint64_t *warmup_array = nullptr;
uint64_t *workload_array = nullptr;

// Operation types
enum op_type : uint8_t { Lookup = 0, Insert = 1, Update = 2, Delete = 3, Range = 4 };
uint64_t op_mask = (1ULL << 56) - 1;

// Random generators
std::default_random_engine rng_engine;
std::uniform_int_distribution<Value> randval(define::kValueMin, define::kValueMax);

Tree *tree;
DSM *dsm;

// ============================================
// Zipfian Generator (simplified)
// ============================================
class ZipfianGenerator {
public:
    ZipfianGenerator(uint64_t n, double theta, uint64_t seed = 0) 
        : n_(n), theta_(theta), gen_(seed == 0 ? std::random_device{}() : seed) {
        // Precompute zeta values
        zeta_n_ = 0;
        for (uint64_t i = 1; i <= n_; ++i) {
            zeta_n_ += 1.0 / std::pow(i, theta_);
        }
        zeta_2_ = 1.0 + std::pow(0.5, theta_);
        alpha_ = 1.0 / (1.0 - theta_);
        eta_ = (1.0 - std::pow(2.0 / n_, 1.0 - theta_)) / (1.0 - zeta_2_ / zeta_n_);
    }

    uint64_t next() {
        double u = dist_(gen_);
        double uz = u * zeta_n_;
        if (uz < 1.0) return 0;
        if (uz < 1.0 + std::pow(0.5, theta_)) return 1;
        return (uint64_t)(n_ * std::pow(eta_ * u - eta_ + 1.0, alpha_));
    }

private:
    uint64_t n_;
    double theta_;
    double zeta_n_;
    double zeta_2_;
    double alpha_;
    double eta_;
    std::mt19937_64 gen_;
    std::uniform_real_distribution<double> dist_{0.0, 1.0};
};

ZipfianGenerator *zipf_gen = nullptr;
std::mt19937_64 uniform_gen;

// ============================================
// Key Generation
// ============================================
inline Key to_key(uint64_t k) {
    // Hash the key and convert to CHIME's Key type (array<uint8_t, 8>)
    uint64_t hashed = (CityHash64((char *)&k, sizeof(k)) + 1) % kKeySpace;
    return int2key(hashed);
}

inline uint64_t generate_key() {
    if (uniform_workload) {
        return uniform_gen() % kKeySpace;
    } else {
        return zipf_gen->next() % kKeySpace;
    }
}

// ============================================
// Latency Recording (500ns buckets)
// ============================================
inline void record_latency(int thread_id, uint64_t latency_ns) {
    uint64_t bucket = latency_ns / LATENCY_NS_GRANULARITY;  // Convert to 500ns buckets
    if (bucket >= LATENCY_BUCKETS) {
        bucket = LATENCY_BUCKETS - 1;
    }
    latency_histogram[thread_id][bucket]++;
    total_latency_samples.fetch_add(1, std::memory_order_relaxed);
}

// ============================================
// Thread Functions
// ============================================
void thread_run(int id) {
    // Use modulo to handle systems with fewer cores
    // Try to spread across available cores
    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int core = (id % num_cpus);
    bindCore(core);
    dsm->registerThread();
    
    tp[id][0] = 0;
    total_tp[id] = 0;
    uint64_t my_id = kMaxThread * dsm->getMyNodeID() + id;
    
    printf("I am thread %lu\n", my_id);
    
    auto thread_id = dsm->getMyThreadID();
    
    // ========== BULK LOAD (only first 8 threads) ==========
    if (id < 8) {
        uint64_t load_per_thread = bulk_load_num / 8;
        uint64_t start_idx = id * load_per_thread;
        uint64_t end_idx = (id == 7) ? bulk_load_num : start_idx + load_per_thread;
        
        printf("Thread %d loading keys %lu to %lu\n", id, start_idx, end_idx);
        
        for (uint64_t i = start_idx; i < end_idx; ++i) {
            Key k = int2key(bulk_array[i]);
            Value v = bulk_array[i];
            tree->insert(k, v);
            
            if ((i - start_idx) % 100000 == 0) {
                printf("Thread %d: loaded %lu keys\n", id, i - start_idx);
            }
        }
        printf("Thread %d: bulk load complete\n", id);
    }
    
    warmup_cnt.fetch_add(1);
    
    // Wait for all threads to finish bulk loading
    if (id == 0) {
        while (warmup_cnt.load() != kThreadCount);
        printf("Node %d: bulk load complete\n", dsm->getMyNodeID());
        // Signal main thread that bulk load is done (barrier called from main)
        warmup_cnt.store(-1);  // Special signal
    }
    // Wait for main thread to complete barrier
    while (warmup_cnt.load() != 0);
    
    // ========== WARMUP PHASE ==========
    uint64_t *my_warmup = warmup_array + id * thread_warmup_num;
    
    Timer timer;
    for (uint64_t i = 0; i < thread_warmup_num; ++i) {
        uint64_t encoded = my_warmup[i];
        op_type op = static_cast<op_type>(encoded >> 56);
        uint64_t raw_key = encoded & op_mask;
        Key k = int2key(raw_key);
        
        switch (op) {
            case Lookup: {
                Value v;
                tree->search(k, v);
            } break;
            case Insert: {
                tree->insert(k, raw_key);
            } break;
            case Update: {
                tree->update(k, raw_key + 1);
            } break;
            default:
                break;
        }
    }
    
    warmup_cnt.fetch_add(1);
    
    if (id == 0) {
        while (warmup_cnt.load() != kThreadCount);
        printf("Node %d: warmup complete\n", dsm->getMyNodeID());
        // Signal main thread that warmup is done (barrier called from main)
        warmup_cnt.store(-2);  // Special signal for warmup done
    }
    // Wait for main thread to complete barrier and signal ready
    while (!ready.load());
    
    // ========== MAIN BENCHMARK WITH LATENCY TRACKING ==========
    // Clear latency histogram for this thread
    memset(latency_histogram[id], 0, sizeof(uint64_t) * LATENCY_BUCKETS);
    
    // Wait for signal to start measurement
    worker.fetch_add(1);
    while (!ready_to_report.load());
    
    uint64_t *my_workload = workload_array + id * thread_op_num;
    uint64_t ops_done = 0;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    for (uint64_t i = 0; i < thread_op_num; ++i) {
        uint64_t encoded = my_workload[i];
        op_type op = static_cast<op_type>(encoded >> 56);
        uint64_t raw_key = encoded & op_mask;
        Key k = int2key(raw_key);
        
        timer.begin();
        
        switch (op) {
            case Lookup: {
                Value v;
                tree->search(k, v);
            } break;
            case Insert: {
                tree->insert(k, raw_key);
            } break;
            case Update: {
                tree->update(k, raw_key + 1);
            } break;
            case Delete: {
                // CHIME doesn't have delete, treat as update
                tree->update(k, raw_key + 1);
            } break;
            case Range: {
                std::map<Key, Value> results;
                tree->range_query(k, k + 100, results);
            } break;
        }
        
        uint64_t latency_ns = timer.end();
        record_latency(thread_id, latency_ns);
        
        tp[id][0]++;
        ops_done++;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    total_tp[id] = (uint64_t)(ops_done / (duration_us / 1e6));
    execute_op.fetch_add(ops_done);
    worker.fetch_sub(1);
    
    printf("Thread %d: completed %lu ops, throughput %.2f Kops/s\n", 
           id, ops_done, total_tp[id] / 1000.0);
}

// ============================================
// Workload Generation
// ============================================
void generate_workload() {
    printf("Generating workload...\n");
    printf("  KeySpace: %lu\n", kKeySpace);
    printf("  Bulk load: %lu\n", bulk_load_num);
    printf("  Warmup: %lu\n", warmup_num);
    printf("  Operations: %lu\n", op_num);
    printf("  Read/Insert/Update/Delete/Range: %d/%d/%d/%d/%d\n",
           kReadRatio, kInsertRatio, kUpdateRatio, kDeleteRatio, kRangeRatio);
    
    // Initialize random generators
    if (uniform_workload) {
        uniform_gen.seed(std::random_device{}() ^ node_id);
    } else {
        zipf_gen = new ZipfianGenerator(kKeySpace, zipfian, std::random_device{}() ^ node_id);
    }
    
    // Generate bulk load array (sequential keys, shuffled)
    bulk_array = new uint64_t[bulk_load_num];
    for (uint64_t i = 0; i < bulk_load_num; ++i) {
        bulk_array[i] = i;
    }
    std::mt19937_64 shuffle_gen(0xc70f6907UL);
    std::shuffle(bulk_array, bulk_array + bulk_load_num, shuffle_gen);
    
    // Calculate per-thread counts
    uint64_t node_warmup_num = warmup_num / CNodeCount;
    uint64_t node_op_num = op_num / CNodeCount;
    thread_warmup_num = node_warmup_num / kThreadCount;
    thread_op_num = node_op_num / kThreadCount;
    
    // Generate warmup array
    warmup_array = new uint64_t[node_warmup_num];
    std::mt19937_64 op_gen(std::random_device{}() ^ node_id);
    std::uniform_int_distribution<uint32_t> op_dist(0, 99);
    
    uint64_t insert_counter = bulk_load_num;
    
    for (uint64_t i = 0; i < node_warmup_num; ++i) {
        uint32_t r = op_dist(op_gen);
        uint64_t key;
        op_type op;
        
        if (r < kReadRatio) {
            op = Lookup;
            key = generate_key() % bulk_load_num;  // Read existing keys
        } else if (r < kReadRatio + kInsertRatio) {
            op = Insert;
            key = insert_counter++;
        } else if (r < kReadRatio + kInsertRatio + kUpdateRatio) {
            op = Update;
            key = generate_key() % bulk_load_num;
        } else if (r < kReadRatio + kInsertRatio + kUpdateRatio + kDeleteRatio) {
            op = Delete;
            key = generate_key() % bulk_load_num;
        } else {
            op = Range;
            key = generate_key() % bulk_load_num;
        }
        
        warmup_array[i] = key | (static_cast<uint64_t>(op) << 56);
    }
    
    // Generate main workload array
    workload_array = new uint64_t[node_op_num];
    
    for (uint64_t i = 0; i < node_op_num; ++i) {
        uint32_t r = op_dist(op_gen);
        uint64_t key;
        op_type op;
        
        if (r < kReadRatio) {
            op = Lookup;
            key = generate_key() % bulk_load_num;
        } else if (r < kReadRatio + kInsertRatio) {
            op = Insert;
            key = insert_counter++;
        } else if (r < kReadRatio + kInsertRatio + kUpdateRatio) {
            op = Update;
            key = generate_key() % bulk_load_num;
        } else if (r < kReadRatio + kInsertRatio + kUpdateRatio + kDeleteRatio) {
            op = Delete;
            key = generate_key() % bulk_load_num;
        } else {
            op = Range;
            key = generate_key() % bulk_load_num;
        }
        
        workload_array[i] = key | (static_cast<uint64_t>(op) << 56);
    }
    
    printf("Workload generation complete\n");
}

// ============================================
// Save Latency Histogram
// ============================================
void save_latency_histogram(const std::string& filename) {
    // Aggregate latency from all threads
    uint64_t total_histogram[LATENCY_BUCKETS];
    memset(total_histogram, 0, sizeof(total_histogram));
    
    for (int t = 0; t < kThreadCount; ++t) {
        for (int i = 0; i < LATENCY_BUCKETS; ++i) {
            total_histogram[i] += latency_histogram[t][i];
        }
    }
    
    // Calculate statistics
    uint64_t total_ops = 0;
    uint64_t sum_latency = 0;
    for (int i = 0; i < LATENCY_BUCKETS; ++i) {
        total_ops += total_histogram[i];
        sum_latency += total_histogram[i] * i;
    }
    
    double avg_latency = (total_ops > 0) ? (double)sum_latency / total_ops : 0;
    
    // Find percentiles
    uint64_t p50_target = total_ops * 0.50;
    uint64_t p90_target = total_ops * 0.90;
    uint64_t p95_target = total_ops * 0.95;
    uint64_t p99_target = total_ops * 0.99;
    uint64_t p999_target = total_ops * 0.999;
    
    uint64_t cumulative = 0;
    uint64_t p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0;
    
    for (int i = 0; i < LATENCY_BUCKETS; ++i) {
        cumulative += total_histogram[i];
        if (p50 == 0 && cumulative >= p50_target) p50 = i;
        if (p90 == 0 && cumulative >= p90_target) p90 = i;
        if (p95 == 0 && cumulative >= p95_target) p95 = i;
        if (p99 == 0 && cumulative >= p99_target) p99 = i;
        if (p999 == 0 && cumulative >= p999_target) p999 = i;
    }
    
    // Print statistics (in nanoseconds for 500ns buckets)
    printf("\n========== CHIME LATENCY STATISTICS (500ns buckets) ==========\n");
    printf("Total operations: %lu\n", total_ops);
    printf("Average latency: %.2f ns\n", avg_latency * LATENCY_NS_GRANULARITY);
    printf("P50 latency: %lu ns\n", p50 * LATENCY_NS_GRANULARITY);
    printf("P90 latency: %lu ns\n", p90 * LATENCY_NS_GRANULARITY);
    printf("P95 latency: %lu ns\n", p95 * LATENCY_NS_GRANULARITY);
    printf("P99 latency: %lu ns\n", p99 * LATENCY_NS_GRANULARITY);
    printf("P99.9 latency: %lu ns\n", p999 * LATENCY_NS_GRANULARITY);
    printf("================================================================\n\n");
    
    // Save to file
    std::ofstream out(filename);
    if (out.is_open()) {
        out << "# CHIME Latency Histogram (nanoseconds, 500ns buckets)\n";
        out << "# Total ops: " << total_ops << "\n";
        out << "# Avg: " << avg_latency * LATENCY_NS_GRANULARITY << " ns\n";
        out << "# P50: " << p50 * LATENCY_NS_GRANULARITY << " ns, P90: " << p90 * LATENCY_NS_GRANULARITY 
            << " ns, P95: " << p95 * LATENCY_NS_GRANULARITY << " ns, P99: " << p99 * LATENCY_NS_GRANULARITY 
            << " ns, P99.9: " << p999 * LATENCY_NS_GRANULARITY << " ns\n";
        out << "# latency_ns\tcount\n";
        
        for (int i = 0; i < LATENCY_BUCKETS; ++i) {
            if (total_histogram[i] > 0) {
                out << (i * LATENCY_NS_GRANULARITY) << "\t" << total_histogram[i] << "\n";
            }
        }
        out.close();
        printf("Latency histogram saved to: %s\n", filename.c_str());
    } else {
        printf("ERROR: Failed to save latency histogram to %s\n", filename.c_str());
    }
}

// ============================================
// Parse Arguments (same as DEX for compatibility)
// ============================================
void parse_args(int argc, char *argv[]) {
    if (argc != 23) {
        printf("Usage: ./microbench_latency kNodeCount kReadRatio kInsertRatio kUpdateRatio "
               "kDeleteRatio kRangeRatio "
               "totalThreadCount memThreadCount "
               "cacheSize(MB) uniform_workload zipfian_theta bulk_load_num(M) "
               "warmup_num(M) op_num(M) "
               "check_correctness time_based early_stop "
               "index rpc_rate admission_rate "
               "auto_tune kMaxThread\n");
        printf("\nExample (100%% reads, Zipfian):\n");
        printf("  ./microbench_latency 2 100 0 0 0 0 16 4 256 0 0.99 10 1 5 0 0 0 0 1.0 1.0 0 16\n");
        exit(-1);
    }
    
    kNodeCount = atoi(argv[1]);
    kReadRatio = atoi(argv[2]);
    kInsertRatio = atoi(argv[3]);
    kUpdateRatio = atoi(argv[4]);
    kDeleteRatio = atoi(argv[5]);
    kRangeRatio = atoi(argv[6]);
    
    if ((kReadRatio + kInsertRatio + kUpdateRatio + kDeleteRatio + kRangeRatio) != 100) {
        printf("ERROR: Ratios must sum to 100\n");
        exit(-1);
    }
    
    int totalThreadCount = atoi(argv[7]);
    memThreadCount = atoi(argv[8]);
    cache_mb = atoi(argv[9]);
    uniform_workload = atoi(argv[10]);
    zipfian = atof(argv[11]);
    bulk_load_num = atoi(argv[12]) * 1000000ULL;
    warmup_num = atoi(argv[13]) * 1000000ULL;
    op_num = atoi(argv[14]) * 1000000ULL;
    check_correctness = atoi(argv[15]);
    time_based = atoi(argv[16]);
    early_stop = atoi(argv[17]);
    tree_index = atoi(argv[18]);
    rpc_rate = atof(argv[19]);
    admission_rate = atof(argv[20]);
    auto_tune = atoi(argv[21]);
    kMaxThread = atoi(argv[22]);
    
    // Calculate derived values
    kKeySpace = bulk_load_num + (uint64_t)std::ceil((op_num + warmup_num) * (kInsertRatio / 100.0)) + 1000;
    
    CNodeCount = (totalThreadCount % kMaxThread == 0) 
                 ? (totalThreadCount / kMaxThread)
                 : (totalThreadCount / kMaxThread + 1);
    
    printf("\n========== CHIME MICROBENCHMARK ==========\n");
    printf("kNodeCount: %d, CNodeCount: %d\n", kNodeCount, CNodeCount);
    printf("Read/Insert/Update/Delete/Range: %d/%d/%d/%d/%d\n",
           kReadRatio, kInsertRatio, kUpdateRatio, kDeleteRatio, kRangeRatio);
    printf("totalThreadCount: %d, kMaxThread: %d\n", totalThreadCount, kMaxThread);
    printf("uniform_workload: %d, zipfian: %.2f\n", uniform_workload, zipfian);
    printf("bulk_load: %luM, warmup: %luM, ops: %luM\n",
           bulk_load_num / 1000000, warmup_num / 1000000, op_num / 1000000);
    printf("KeySpace: %lu\n", kKeySpace);
    printf("==========================================\n\n");
}

// ============================================
// Main - ALL nodes run same code (like ycsb_test.cpp)
// ============================================
int main(int argc, char *argv[]) {
    parse_args(argc, argv);
    
    // Initialize latency histograms
    memset(latency_histogram, 0, sizeof(latency_histogram));
    
    // Initialize DSM - same for all nodes
    DSMConfig config;
    config.machineNR = kNodeCount;
    config.threadNR = kMaxThread;
    dsm = DSM::getInstance(config);
    
    node_id = dsm->getMyNodeID();
    kThreadCount = kMaxThread;
    
    printf("Node %d starting (total nodes: %d)\n", node_id, kNodeCount);
    
    // Bind core and register thread - same for all nodes
    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int core_id = (kThreadCount * 2 + 1) % num_cpus;
    bindCore(core_id);
    dsm->registerThread();
    
    // Create tree - ALL nodes create tree, but only node 0 initializes root
    tree = new Tree(dsm);
    printf("Node %d: Tree created\n", node_id);
    
    // Generate workload - ALL nodes generate workload
    generate_workload();
    
    // Initial barrier - ALL nodes must reach this
    dsm->barrier("benchmark");
    printf("Node %d: initial barrier passed\n", node_id);
    
    // Launch threads
    for (int i = 0; i < kThreadCount; ++i) {
        th[i] = std::thread(thread_run, i);
    }
    
    // Wait for bulk load to complete (signaled by warmup_cnt == -1)
    while (warmup_cnt.load() != -1) {
        usleep(1000);
    }
    dsm->barrier("load_finish");  // Sync after bulk load
    printf("Node %d: bulk load barrier passed\n", node_id);
    warmup_cnt.store(0);  // Allow threads to proceed
    
    // Wait for warmup to complete (signaled by warmup_cnt == -2)
    while (warmup_cnt.load() != -2) {
        usleep(1000);
    }
    dsm->barrier("warmup_finish");  // Sync after warmup
    printf("Node %d: warmup barrier passed\n", node_id);
    ready.store(true);  // Signal threads to start measurement
    
    // Wait for all threads to be ready for measurement
    while (worker.load() != (uint64_t)kThreadCount) {
        usleep(100);
    }
    
    printf("Node %d: All threads ready, starting measurement...\n", node_id);
    ready_to_report.store(true);
    
    // Wait for completion
    for (int i = 0; i < kThreadCount; ++i) {
        th[i].join();
    }
    
    // Calculate total throughput
    uint64_t total_throughput = 0;
    for (int i = 0; i < kThreadCount; ++i) {
        total_throughput += total_tp[i];
    }
    
    printf("\n========== RESULTS ==========\n");
    printf("Node %d throughput: %.2f Mops/s\n", node_id, total_throughput / 1e6);
    
    // Save latency on compute node only (node 1 in 2-node setup)
    // Node 0 is memory server, node 1 is where we measure latency
    if (node_id == 1) {
        save_latency_histogram("chime_latency.dat");
    }
    
    dsm->barrier("fin");  // Final sync
    
    // Cleanup
    if (bulk_array) delete[] bulk_array;
    if (warmup_array) delete[] warmup_array;
    if (workload_array) delete[] workload_array;
    if (zipf_gen) delete zipf_gen;
    
    printf("Node %d: [END]\n", node_id);
    return 0;
}
