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
#define LATENCY_BUCKETS 100000

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
// Record latency
// ============================================
inline void record_latency(int thread_id, uint64_t latency_ns) {
    uint64_t latency_us = latency_ns / 1000;
    if (latency_us >= LATENCY_BUCKETS) {
        latency_us = LATENCY_BUCKETS - 1;
    }
    latency_histogram[thread_id][latency_us]++;
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
    
    // ========== BULK LOAD (only first 8 threads on node 0) ==========
    if (dsm->getMyNodeID() == 0 && id < 8) {
        uint64_t load_per_thread = bulk_load_num / 8;
        uint64_t start_idx = id * load_per_thread;
        uint64_t end_idx = (id == 7) ? bulk_load_num : start_idx + load_per_thread;
        
        printf("Thread %d loading keys %lu to %lu\n", id, start_idx, end_idx);
        
        for (uint64_t i = start_idx; i < end_idx; ++i) {
            Key k = int2key(i);
            tree->insert(k, i);
            
            if ((i - start_idx) % 500000 == 0 && i > start_idx) {
                printf("Thread %d: loaded %lu keys\n", id, i - start_idx);
            }
        }
        printf("Thread %d: bulk load complete\n", id);
    }
    
    warmup_cnt.fetch_add(1);
    while (warmup_cnt.load() < kThreadCount);
    
    // Wait for bulk load to finish on all nodes
    if (id == 0) {
        dsm->barrier("load_finish");
        printf("Node %d: bulk load barrier passed\n", dsm->getMyNodeID());
    }
    
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
    
    warmup_cnt.fetch_add(1);
    while (warmup_cnt.load() < kThreadCount * 2);
    
    if (id == 0) {
        dsm->barrier("warmup_finish");
        printf("Node %d: warmup barrier passed\n", dsm->getMyNodeID());
        ready.store(true);
    }
    while (!ready.load());
    
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
// Save latency histogram
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
        sum_latency += total_histogram[i] * i;
    }
    
    double avg_latency = (total_ops > 0) ? (double)sum_latency / total_ops : 0;
    
    uint64_t cumulative = 0;
    uint64_t p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0;
    
    for (int i = 0; i < LATENCY_BUCKETS; ++i) {
        cumulative += total_histogram[i];
        if (p50 == 0 && cumulative >= total_ops * 0.50) p50 = i;
        if (p90 == 0 && cumulative >= total_ops * 0.90) p90 = i;
        if (p95 == 0 && cumulative >= total_ops * 0.95) p95 = i;
        if (p99 == 0 && cumulative >= total_ops * 0.99) p99 = i;
        if (p999 == 0 && cumulative >= total_ops * 0.999) p999 = i;
    }
    
    printf("\n========== CHIME LATENCY STATISTICS ==========\n");
    printf("Total operations: %lu\n", total_ops);
    printf("Average latency: %.2f us\n", avg_latency);
    printf("P50: %lu us, P90: %lu us, P95: %lu us, P99: %lu us, P99.9: %lu us\n",
           p50, p90, p95, p99, p999);
    printf("==============================================\n\n");
    
    std::ofstream out(filename);
    if (out.is_open()) {
        out << "# CHIME Latency Histogram\n";
        out << "# Total ops: " << total_ops << "\n";
        out << "# Avg: " << avg_latency << " us\n";
        out << "# P50: " << p50 << " P90: " << p90 << " P95: " << p95 
            << " P99: " << p99 << " P99.9: " << p999 << " us\n";
        out << "# latency_us\tcount\n";
        
        for (int i = 0; i < LATENCY_BUCKETS; ++i) {
            if (total_histogram[i] > 0) {
                out << i << "\t" << total_histogram[i] << "\n";
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
    
    kKeySpace = bulk_load_num + 1000;
    
    printf("\n========== CHIME BENCHMARK ==========\n");
    printf("Nodes: %d, Threads: %d\n", kNodeCount, kThreadCount);
    printf("Read ratio: %d%%, Zipfian: %.2f\n", kReadRatio, zipfian_theta);
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
