/**
 * CHIME Latency Benchmark with 500ns Buckets
 * 
 * This benchmark runs reads and range scans with latency tracking
 * using 500ns granularity buckets (same as DEX).
 * 
 * Usage: ./latency_bench <node_count> <thread_count> <read_ratio> <range_ratio> <total_ops> [range_size]
 * 
 * Example for 2 nodes, 1 thread, 70% reads, 30% range scans, 1M ops:
 *   ./latency_bench 2 1 70 30 1000000
 */

#include "Tree.h"
#include "Timer.h"
#include "zipf.h"
#include <city.h>

#include <stdlib.h>
#include <thread>
#include <time.h>
#include <vector>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <random>
#include <atomic>
#include <chrono>
#include <map>

// ============================================================================
// Configuration - 500ns latency buckets like DEX
// ============================================================================
#define LATENCY_NS_GRANULARITY 500    // 500 nanoseconds per bucket
#define LATENCY_BUCKETS 100000        // Up to 50ms (100000 * 500ns)
#define WARMUP_OPS 10000              // Warmup operations before measurement
#define BULK_LOAD_COUNT 100000        // Keys to bulk load before benchmark
#define ZIPF_THETA 0.99               // Zipfian skew factor

// Per-thread latency histograms
uint64_t read_latency[MAX_APP_THREAD][LATENCY_BUCKETS];
uint64_t range_latency[MAX_APP_THREAD][LATENCY_BUCKETS];

// Counters
std::atomic<uint64_t> total_reads{0};
std::atomic<uint64_t> total_ranges{0};
std::atomic<uint64_t> completed_ops{0};
std::atomic<bool> benchmark_done{false};

// Configuration
int kNodeCount = 2;
int kThreadCount = 1;
int kReadRatio = 70;
int kRangeRatio = 30;
uint64_t kTotalOps = 1000000;
int kRangeSize = 50;
uint64_t kKeySpace = BULK_LOAD_COUNT;

Tree *tree;
DSM *dsm;

// Zipfian generator (from zipf.h)
struct zipf_gen_state zipf_state;

/**
 * Record latency in appropriate histogram bucket
 */
inline void record_latency(uint64_t* histogram, uint64_t ns) {
    uint64_t bucket = ns / LATENCY_NS_GRANULARITY;
    if (bucket >= LATENCY_BUCKETS) {
        bucket = LATENCY_BUCKETS - 1;
    }
    histogram[bucket]++;
}

/**
 * Bulk load initial keys (only on memory node / node 0)
 */
void bulk_load(int thread_id) {
    printf("Thread %d: Bulk loading %lu keys...\n", thread_id, kKeySpace);
    
    for (uint64_t i = thread_id; i < kKeySpace; i += kThreadCount) {
        Key k = int2key(i);
        tree->insert(k, i + 1);  // Value = key + 1
        
        if (i > 0 && i % 10000 == 0) {
            printf("Thread %d: Loaded %lu keys\n", thread_id, i);
        }
    }
    printf("Thread %d: Bulk load complete\n", thread_id);
}

/**
 * Worker thread function
 */
void worker_thread(int thread_id, uint64_t ops_per_thread) {
    bindCore(thread_id);
    dsm->registerThread();
    
    // Initialize per-thread Zipfian state
    struct zipf_gen_state local_zipf;
    mehcached_zipf_init(&local_zipf, kKeySpace, ZIPF_THETA, thread_id * 12345);
    
    std::mt19937 rng(thread_id * 54321);
    std::uniform_int_distribution<int> op_dist(1, 100);
    
    Timer timer;
    Value v;
    
    printf("Thread %d: Starting benchmark with %lu ops\n", thread_id, ops_per_thread);
    
    // Warmup phase
    for (uint64_t i = 0; i < WARMUP_OPS && !benchmark_done; i++) {
        uint64_t key_idx = mehcached_zipf_next(&local_zipf) % kKeySpace;
        Key k = int2key(key_idx);
        tree->search(k, v);
    }
    
    // Measurement phase
    for (uint64_t i = 0; i < ops_per_thread && !benchmark_done; i++) {
        int op_choice = op_dist(rng);
        uint64_t key_idx = mehcached_zipf_next(&local_zipf) % kKeySpace;
        Key k = int2key(key_idx);
        
        if (op_choice <= kReadRatio) {
            // Point read (lookup)
            timer.begin();
            tree->search(k, v);
            uint64_t elapsed_ns = timer.end();
            
            record_latency(read_latency[thread_id], elapsed_ns);
            total_reads++;
        } else {
            // Range scan - scan from key to key+range_size
            Key to_key = int2key(key_idx + kRangeSize);
            std::map<Key, Value> results;
            
            timer.begin();
            tree->range_query(k, to_key, results);
            uint64_t elapsed_ns = timer.end();
            
            record_latency(range_latency[thread_id], elapsed_ns);
            total_ranges++;
        }
        
        completed_ops++;
        
        if (completed_ops % 100000 == 0) {
            printf("Progress: %lu / %lu ops\n", completed_ops.load(), kTotalOps);
        }
    }
    
    printf("Thread %d: Completed\n", thread_id);
}

/**
 * Save latency histogram to file
 */
void save_latency_histogram(const char* filename, uint64_t histogram[][LATENCY_BUCKETS], 
                            int thread_count, const char* op_type) {
    // Aggregate across threads
    uint64_t total[LATENCY_BUCKETS] = {0};
    uint64_t total_samples = 0;
    
    for (int t = 0; t < thread_count; t++) {
        for (int b = 0; b < LATENCY_BUCKETS; b++) {
            total[b] += histogram[t][b];
            total_samples += histogram[t][b];
        }
    }
    
    if (total_samples == 0) {
        printf("No %s samples to save\n", op_type);
        return;
    }
    
    std::ofstream file(filename);
    // Simple .dat format: latency_ns TAB count (easy to read with cat)
    file << "# CHIME Latency Histogram - " << op_type << std::endl;
    file << "# Bucket size: " << LATENCY_NS_GRANULARITY << " ns" << std::endl;
    file << "# Total samples: " << total_samples << std::endl;
    file << "# Format: latency_ns\tcount" << std::endl;
    
    uint64_t cumulative = 0;
    for (int b = 0; b < LATENCY_BUCKETS; b++) {
        if (total[b] > 0) {
            cumulative += total[b];
            uint64_t latency_ns = (uint64_t)b * LATENCY_NS_GRANULARITY;
            file << latency_ns << "\t" << total[b] << std::endl;
        }
    }
    
    file.close();
    printf("Saved %s latency histogram to %s (%lu samples)\n", op_type, filename, total_samples);
    
    // Print percentiles
    printf("\n%s Latency Percentiles:\n", op_type);
    double percentiles[] = {50, 90, 95, 99, 99.9};
    cumulative = 0;
    int pct_idx = 0;
    for (int b = 0; b < LATENCY_BUCKETS && pct_idx < 5; b++) {
        cumulative += total[b];
        double pct = 100.0 * cumulative / total_samples;
        while (pct_idx < 5 && pct >= percentiles[pct_idx]) {
            uint64_t latency_ns = (uint64_t)b * LATENCY_NS_GRANULARITY;
            printf("  P%.1f: %.2f us\n", percentiles[pct_idx], latency_ns / 1000.0);
            pct_idx++;
        }
    }
}

void print_usage(const char* prog) {
    printf("Usage: %s <node_count> <thread_count> <read_ratio> <range_ratio> <total_ops> [range_size]\n", prog);
    printf("\nExample for 2 nodes, 1 thread, 70%% reads, 30%% range scans, 1M ops:\n");
    printf("  %s 2 1 70 30 1000000\n", prog);
    printf("\nArguments:\n");
    printf("  node_count   - Total nodes (1 memory + N-1 compute)\n");
    printf("  thread_count - Threads per compute node\n");
    printf("  read_ratio   - Percentage of point reads (0-100)\n");
    printf("  range_ratio  - Percentage of range scans (0-100)\n");
    printf("  total_ops    - Total operations to run\n");
    printf("  range_size   - Range scan size (default: 50)\n");
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        print_usage(argv[0]);
        return 1;
    }
    
    kNodeCount = atoi(argv[1]);
    kThreadCount = atoi(argv[2]);
    kReadRatio = atoi(argv[3]);
    kRangeRatio = atoi(argv[4]);
    kTotalOps = atoll(argv[5]);
    if (argc > 6) kRangeSize = atoi(argv[6]);
    
    if (kReadRatio + kRangeRatio != 100) {
        printf("Error: read_ratio + range_ratio must equal 100\n");
        return 1;
    }
    
    printf("===========================================\n");
    printf("CHIME Latency Benchmark\n");
    printf("===========================================\n");
    printf("Node count:    %d\n", kNodeCount);
    printf("Thread count:  %d\n", kThreadCount);
    printf("Read ratio:    %d%%\n", kReadRatio);
    printf("Range ratio:   %d%%\n", kRangeRatio);
    printf("Total ops:     %lu\n", kTotalOps);
    printf("Range size:    %d\n", kRangeSize);
    printf("Latency bucket: %d ns\n", LATENCY_NS_GRANULARITY);
    printf("===========================================\n");
    
    // Initialize latency histograms
    memset(read_latency, 0, sizeof(read_latency));
    memset(range_latency, 0, sizeof(range_latency));
    
    // Initialize DSM
    DSMConfig config;
    config.machineNR = kNodeCount;
    config.threadNR = kThreadCount;
    
    printf("Initializing DSM...\n");
    dsm = DSM::getInstance(config);
    
    int my_node = dsm->getMyNodeID();
    printf("Node %d initialized (memory nodes: %d)\n", my_node, MEMORY_NODE_NUM);
    
    // Register main thread
    bindCore(0);
    dsm->registerThread();
    
    // Create tree
    printf("Node %d: Creating B+ tree...\n", my_node);
    tree = new Tree(dsm);
    
    // Synchronize
    dsm->barrier("tree_init");
    printf("Node %d: Tree initialization complete\n", my_node);
    
    // Bulk load on compute nodes (not memory node)
    if (my_node >= MEMORY_NODE_NUM) {
        // Only first compute node bulk loads (single-threaded for simplicity)
        if (my_node == MEMORY_NODE_NUM) {
            printf("Node %d: Starting bulk load (%lu keys)...\n", my_node, kKeySpace);
            
            for (uint64_t i = 0; i < kKeySpace; i++) {
                Key k = int2key(i);
                tree->insert(k, i + 1);  // Value = key + 1
                
                if ((i + 1) % 10000 == 0) {
                    printf("  Loaded %lu / %lu keys\n", i + 1, kKeySpace);
                }
            }
            printf("Node %d: Bulk load complete\n", my_node);
        }
    }
    
    dsm->barrier("bulk_load");
    printf("Node %d: All nodes synchronized after bulk load\n", my_node);
    
    // Run benchmark on compute nodes (single-threaded for simplicity)
    if (my_node >= MEMORY_NODE_NUM) {
        printf("Node %d: Starting benchmark (%lu ops)...\n", my_node, kTotalOps);
        
        // Initialize Zipfian generator
        struct zipf_gen_state local_zipf;
        mehcached_zipf_init(&local_zipf, kKeySpace, ZIPF_THETA, 12345);
        
        std::mt19937 rng(54321);
        std::uniform_int_distribution<int> op_dist(1, 100);
        
        Timer timer;
        Value v;
        int thread_id = 0;  // Main thread
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Warmup
        printf("  Warmup: %d ops\n", WARMUP_OPS);
        for (int i = 0; i < WARMUP_OPS; i++) {
            uint64_t key_idx = mehcached_zipf_next(&local_zipf) % kKeySpace;
            Key k = int2key(key_idx);
            tree->search(k, v);
        }
        
        // Reset histograms after warmup
        memset(read_latency, 0, sizeof(read_latency));
        memset(range_latency, 0, sizeof(range_latency));
        
        // Measurement
        printf("  Running %lu operations...\n", kTotalOps);
        for (uint64_t i = 0; i < kTotalOps; i++) {
            int op_choice = op_dist(rng);
            uint64_t key_idx = mehcached_zipf_next(&local_zipf) % kKeySpace;
            Key k = int2key(key_idx);
            
            if (op_choice <= kReadRatio) {
                // Point read (lookup)
                timer.begin();
                tree->search(k, v);
                uint64_t elapsed_ns = timer.end();
                
                record_latency(read_latency[thread_id], elapsed_ns);
                total_reads++;
            } else {
                // Range scan
                Key to_key = int2key(key_idx + kRangeSize);
                std::map<Key, Value> results;
                
                timer.begin();
                tree->range_query(k, to_key, results);
                uint64_t elapsed_ns = timer.end();
                
                record_latency(range_latency[thread_id], elapsed_ns);
                total_ranges++;
            }
            
            completed_ops++;
            
            if ((i + 1) % 100000 == 0) {
                printf("  Progress: %lu / %lu ops\n", i + 1, kTotalOps);
            }
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        printf("\n===========================================\n");
        printf("Benchmark Complete!\n");
        printf("===========================================\n");
        printf("Total time:     %ld ms\n", duration.count());
        printf("Total ops:      %lu\n", completed_ops.load());
        printf("Total reads:    %lu\n", total_reads.load());
        printf("Total ranges:   %lu\n", total_ranges.load());
        if (duration.count() > 0) {
            printf("Throughput:     %.2f ops/sec\n", 
                   completed_ops.load() * 1000.0 / duration.count());
        }
        printf("===========================================\n");
        
        // Save latency histograms
        save_latency_histogram("chime_read_latency.dat", read_latency, 1, "Read");
        save_latency_histogram("chime_range_latency.dat", range_latency, 1, "Range");
    }
    
    dsm->barrier("done");
    printf("Node %d: Benchmark finished\n", my_node);
    
    return 0;
}
