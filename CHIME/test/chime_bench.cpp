/**
 * CHIME Microbenchmark with Latency Tracking
 * 
 * Simple benchmark that works exactly like CHIME's ycsb_test:
 * - ALL nodes run the same code
 * - ALL nodes participate in barriers
 * - Uses Zipfian key generation
 * - Records latency histogram
 * 
 * Usage: ./chime_bench <kNodeCount> <kThreadCount> <read_ratio> <zipfian_theta> <bulk_load_M> <op_num_M>
 * Example: ./chime_bench 2 16 100 0.99 10 5
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
#include <vector>

// Configuration
#define TEST_EPOCH 10
#define TIME_INTERVAL 1.0
#define LATENCY_BUCKETS 200000    // 0-100ms with 500ns granularity
#define LATENCY_NS_PER_BUCKET 500 // 500ns per bucket

// External variables from CHIME
extern double cache_miss[MAX_APP_THREAD];
extern double cache_hit[MAX_APP_THREAD];
extern uint64_t lock_fail[MAX_APP_THREAD];
extern uint64_t try_write_op[MAX_APP_THREAD];
extern uint64_t try_read_op[MAX_APP_THREAD];
extern volatile bool need_stop;

// Global config
int kNodeCount;
int kThreadCount;
uint32_t kReadRatio = 100;
uint32_t kRangeRatio = 0;  // Percentage of range queries
double zipfian_theta = 0.99;
uint64_t bulk_load_num = 10000000;
uint64_t op_num = 5000000;

// Derived
uint64_t kKeySpace;

// Thread management
std::thread th[MAX_APP_THREAD];
uint64_t tp[MAX_APP_THREAD][8];

// Latency histogram
uint64_t latency_histogram[MAX_APP_THREAD][LATENCY_BUCKETS];

// Synchronization
std::atomic<int64_t> warmup_cnt{0};
std::atomic_bool ready{false};

// Random
std::default_random_engine rng_engine;
std::uniform_int_distribution<Value> randval(define::kValueMin, define::kValueMax);

Tree *tree;
DSM *dsm;

// ============================================
// Zipfian Generator
// ============================================
class ZipfianGenerator {
public:
    ZipfianGenerator(uint64_t n, double theta, uint64_t seed = 0) 
        : n_(n), theta_(theta), gen_(seed == 0 ? std::random_device{}() : seed) {
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
    double zeta_n_, zeta_2_, alpha_, eta_;
    std::mt19937_64 gen_;
    std::uniform_real_distribution<double> dist_{0.0, 1.0};
};

ZipfianGenerator *zipf_gen = nullptr;

// ============================================
// Record latency (500ns granularity)
// ============================================
inline void record_latency(int thread_id, uint64_t latency_ns) {
    uint64_t bucket = latency_ns / LATENCY_NS_PER_BUCKET;
    if (bucket >= LATENCY_BUCKETS) {
        bucket = LATENCY_BUCKETS - 1;
    }
    latency_histogram[thread_id][bucket]++;
}

// ============================================
// Thread function
// ============================================
void thread_run(int id) {
    bindCore(id * 2 + 1);
    dsm->registerThread();
    
    uint64_t all_thread = kThreadCount * kNodeCount;
    uint64_t my_id = kThreadCount * dsm->getMyNodeID() + id;
    
    printf("Thread %d (global %lu) started\n", id, my_id);
    
    // Clear latency histogram
    memset(latency_histogram[id], 0, sizeof(uint64_t) * LATENCY_BUCKETS);
    
    // ========== PARALLEL BULK LOAD (all threads on node 1 participate) ==========
    // Node 0 is memory server, Node 1 is compute node
    // All threads on Node 1 load in parallel for ~30x speedup
    if (dsm->getMyNodeID() == 1) {
        uint64_t keys_per_thread = bulk_load_num / kThreadCount;
        uint64_t start_key = id * keys_per_thread;
        uint64_t end_key = (id == kThreadCount - 1) ? bulk_load_num : start_key + keys_per_thread;
        
        printf("Thread %d loading keys [%lu, %lu) (%lu keys)...\n", 
               id, start_key, end_key, end_key - start_key);
        
        for (uint64_t i = start_key; i < end_key; ++i) {
            Key k = int2key(i);
            tree->insert(k, i);
            
            if ((i - start_key) % 500000 == 0 && i > start_key) {
                printf("Thread %d: loaded %lu / %lu keys (%.1f%%)\n", 
                       id, i - start_key, end_key - start_key, 
                       100.0 * (i - start_key) / (end_key - start_key));
            }
        }
        printf("Thread %d: bulk load complete (%lu keys)\n", id, end_key - start_key);
    }
    
    // LOCAL barrier: wait for all local threads to finish loading
    warmup_cnt.fetch_add(1);
    while (warmup_cnt.load() < kThreadCount);
    
    // GLOBAL barrier: only main thread (id 0) does the cross-node sync
    if (id == 0) {
        printf("Node %d: waiting at load_finish barrier...\n", dsm->getMyNodeID());
        dsm->barrier("load_finish");
        printf("Node %d: bulk load barrier passed\n", dsm->getMyNodeID());
        ready.store(true);  // Signal other local threads
    }
    while (!ready.load());
    
    // Simple warmup - all threads do some reads
    if (zipf_gen == nullptr) {
        zipf_gen = new ZipfianGenerator(bulk_load_num, zipfian_theta, 
                                        std::random_device{}() ^ my_id);
    }
    
    uint64_t warmup_ops = 100000 / kThreadCount;
    for (uint64_t i = 0; i < warmup_ops; ++i) {
        uint64_t key_idx = zipf_gen->next() % bulk_load_num;
        Key k = int2key(key_idx);
        Value v;
        tree->search(k, v);
    }
    
    // LOCAL barrier: wait for all local threads to finish warmup
    warmup_cnt.fetch_add(1);
    while (warmup_cnt.load() < kThreadCount * 2);
    
    // GLOBAL barrier for warmup_finish
    if (id == 0) {
        printf("Node %d: waiting at warmup_finish barrier...\n", dsm->getMyNodeID());
        dsm->barrier("warmup_finish");
        printf("Node %d: warmup barrier passed, starting benchmark\n", dsm->getMyNodeID());
    }
    
    // LOCAL barrier before starting benchmark
    warmup_cnt.fetch_add(1);
    while (warmup_cnt.load() < kThreadCount * 3);
    
    // ========== MAIN BENCHMARK ==========
    Timer timer;
    uint64_t thread_ops = op_num / (kThreadCount * kNodeCount);
    uint64_t ops_done = 0;
    
    std::mt19937_64 op_gen(std::random_device{}() ^ my_id);
    std::uniform_int_distribution<uint32_t> op_dist(0, 99);
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    for (uint64_t i = 0; i < thread_ops && !need_stop; ++i) {
        uint64_t key_idx = zipf_gen->next() % bulk_load_num;
        Key k = int2key(key_idx);
        
        uint32_t r = op_dist(op_gen);
        
        timer.begin();
        
        if (r < kReadRatio) {
            // Read
            Value v;
            tree->search(k, v);
        } else if (r < kReadRatio + kRangeRatio) {
            // Range query
            std::map<Key, Value> results;
            tree->range_query(k, k + 100, results);
        } else {
            // Update
            tree->update(k, key_idx + 1);
        }
        
        uint64_t latency_ns = timer.end();
        record_latency(id, latency_ns);
        
        tp[id][0]++;
        ops_done++;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    
    double throughput = ops_done / (duration_us / 1e6);
    printf("Thread %d: %lu ops, %.2f Kops/s\n", id, ops_done, throughput / 1000.0);
}

// ============================================
// Save latency histogram (500ns granularity)
// ============================================
void save_latency_histogram(const std::string& filename) {
    uint64_t total_histogram[LATENCY_BUCKETS];
    memset(total_histogram, 0, sizeof(total_histogram));
    
    for (int t = 0; t < kThreadCount; ++t) {
        for (int i = 0; i < LATENCY_BUCKETS; ++i) {
            total_histogram[i] += latency_histogram[t][i];
        }
    }
    
    uint64_t total_ops = 0;
    uint64_t sum_latency = 0;
    for (int i = 0; i < LATENCY_BUCKETS; ++i) {
        total_ops += total_histogram[i];
        sum_latency += total_histogram[i] * i;  // in buckets
    }
    
    // Convert to microseconds for display (bucket * 500ns / 1000 = bucket * 0.5us)
    double avg_latency_us = (total_ops > 0) ? (double)sum_latency * LATENCY_NS_PER_BUCKET / 1000.0 / total_ops : 0;
    
    uint64_t cumulative = 0;
    uint64_t p50_bucket = 0, p90_bucket = 0, p95_bucket = 0, p99_bucket = 0, p999_bucket = 0;
    
    for (int i = 0; i < LATENCY_BUCKETS; ++i) {
        cumulative += total_histogram[i];
        if (p50_bucket == 0 && cumulative >= total_ops * 0.50) p50_bucket = i;
        if (p90_bucket == 0 && cumulative >= total_ops * 0.90) p90_bucket = i;
        if (p95_bucket == 0 && cumulative >= total_ops * 0.95) p95_bucket = i;
        if (p99_bucket == 0 && cumulative >= total_ops * 0.99) p99_bucket = i;
        if (p999_bucket == 0 && cumulative >= total_ops * 0.999) p999_bucket = i;
    }
    
    // Convert buckets to microseconds
    double p50_us = p50_bucket * LATENCY_NS_PER_BUCKET / 1000.0;
    double p90_us = p90_bucket * LATENCY_NS_PER_BUCKET / 1000.0;
    double p95_us = p95_bucket * LATENCY_NS_PER_BUCKET / 1000.0;
    double p99_us = p99_bucket * LATENCY_NS_PER_BUCKET / 1000.0;
    double p999_us = p999_bucket * LATENCY_NS_PER_BUCKET / 1000.0;
    
    printf("\n========== CHIME LATENCY STATISTICS ==========\n");
    printf("Total operations: %lu\n", total_ops);
    printf("Average latency: %.2f us\n", avg_latency_us);
    printf("P50: %.1f us, P90: %.1f us, P95: %.1f us, P99: %.1f us, P99.9: %.1f us\n",
           p50_us, p90_us, p95_us, p99_us, p999_us);
    printf("==============================================\n\n");
    
    std::ofstream out(filename);
    if (out.is_open()) {
        out << "# CHIME Latency Histogram (500ns granularity)\n";
        out << "# Total ops: " << total_ops << "\n";
        out << "# Avg: " << avg_latency_us << " us\n";
        out << "# P50: " << p50_us << " us, P90: " << p90_us 
            << " us, P95: " << p95_us << " us, P99: " << p99_us 
            << " us, P99.9: " << p999_us << " us\n";
        out << "# latency_ns\tcount\n";
        
        for (int i = 0; i < LATENCY_BUCKETS; ++i) {
            if (total_histogram[i] > 0) {
                // Output actual nanoseconds (bucket * 500ns)
                out << (i * LATENCY_NS_PER_BUCKET) << "\t" << total_histogram[i] << "\n";
            }
        }
        out.close();
        printf("Saved latency to: %s\n", filename.c_str());
    }
}

// ============================================
// Main
// ============================================
int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: ./chime_bench <kNodeCount> <kThreadCount> [read_ratio] [zipfian] [bulk_M] [ops_M]\n");
        printf("Example: ./chime_bench 2 16 100 0.99 10 5\n");
        exit(-1);
    }
    
    kNodeCount = atoi(argv[1]);
    kThreadCount = atoi(argv[2]);
    if (argc > 3) kReadRatio = atoi(argv[3]);
    if (argc > 4) zipfian_theta = atof(argv[4]);
    if (argc > 5) bulk_load_num = atoi(argv[5]) * 1000000ULL;
    if (argc > 6) op_num = atoi(argv[6]) * 1000000ULL;
    if (argc > 7) kRangeRatio = atoi(argv[7]);
    
    kKeySpace = bulk_load_num + 1000;
    
    printf("\n========== CHIME BENCHMARK ==========\n");
    printf("Nodes: %d, Threads: %d\n", kNodeCount, kThreadCount);
    printf("Read: %d%%, Range: %d%%, Update: %d%%, Zipfian: %.2f\n", 
           kReadRatio, kRangeRatio, 100 - kReadRatio - kRangeRatio, zipfian_theta);
    printf("Bulk load: %luM, Operations: %luM\n", bulk_load_num/1000000, op_num/1000000);
    printf("=====================================\n\n");
    
    // Initialize DSM
    DSMConfig config;
    config.machineNR = kNodeCount;
    config.threadNR = kThreadCount;
    dsm = DSM::getInstance(config);
    
    bindCore(kThreadCount * 2 + 1);
    dsm->registerThread();
    
    // Create tree
    tree = new Tree(dsm);
    
    // Initialize Zipfian generator
    zipf_gen = new ZipfianGenerator(bulk_load_num, zipfian_theta, 
                                     std::random_device{}() ^ dsm->getMyNodeID());
    
    // Initial barrier - ALL nodes must reach this
    dsm->barrier("benchmark");
    printf("Node %d: initial barrier passed\n", dsm->getMyNodeID());
    
    // Launch threads
    for (int i = 0; i < kThreadCount; ++i) {
        th[i] = std::thread(thread_run, i);
    }
    
    // Wait for completion
    for (int i = 0; i < kThreadCount; ++i) {
        th[i].join();
    }
    
    // Calculate throughput
    uint64_t total_ops = 0;
    for (int i = 0; i < kThreadCount; ++i) {
        total_ops += tp[i][0];
    }
    
    printf("Node %d: %lu total ops\n", dsm->getMyNodeID(), total_ops);
    
    // Save latency on node 0 only (where data is)
    if (dsm->getMyNodeID() == 0) {
        save_latency_histogram("chime_latency.dat");
    }
    
    // Final barrier
    dsm->barrier("fin");
    
    printf("[END]\n");
    return 0;
}
