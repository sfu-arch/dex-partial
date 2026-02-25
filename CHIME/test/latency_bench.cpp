/**
 * CHIME Latency Benchmark with 500ns Buckets
 * 
 * MATCHED TO DEX:
 *   - CityHash key derivation (same as DEX to_key())
 *   - Pre-generated + shuffled workload arrays
 *   - Full-mix warmup (reads + ranges, not read-only)
 *   - Count-based range scan (kRangeSize consecutive keys from start)
 *   - Separate per-op latency histograms (read vs range)
 *   - No atomic contention in hot path
 * 
 * Usage: ./latency_bench <node_count> <thread_count> <read_ratio> <range_ratio> <total_ops> [range_size]
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
#include <algorithm>
#include <cstring>

// ============================================================================
// Configuration - 500ns latency buckets like DEX
// MATCHED TO DEX: BULK_LOAD_M=10, WARMUP_M=1
// ============================================================================
#define LATENCY_NS_GRANULARITY 500    // 500 nanoseconds per bucket
#define LATENCY_BUCKETS 100000        // Up to 50ms (100000 * 500ns)
#define WARMUP_OPS 1000000            // 1M warmup operations (DEX WARMUP_M=1)
#define KEY_SPACE_PADDING 1000        // DEX adds +1000 to kKeySpace

// Bulk load count - configurable via command line (default 10M)
uint64_t bulk_load_count = 10000000;

// Per-thread latency histograms
uint64_t read_latency[MAX_APP_THREAD][LATENCY_BUCKETS];
uint64_t range_latency[MAX_APP_THREAD][LATENCY_BUCKETS];

// Per-thread counters (NOT atomic — avoids contention, aggregated after join)
uint64_t thread_read_count[MAX_APP_THREAD];
uint64_t thread_range_count[MAX_APP_THREAD];

// Per-thread throughput (DEX-style: each thread measures its own)
uint64_t thread_throughput[MAX_APP_THREAD];

// Thread synchronization — matches DEX's warmup_cnt + ready pattern
std::atomic<int64_t> warmup_cnt{0};
std::atomic<bool> ready{false};

// CHIME diagnostic counters (extern from Tree.cpp)
extern double cache_miss[MAX_APP_THREAD];
extern double cache_hit[MAX_APP_THREAD];
extern uint64_t read_handover_num[MAX_APP_THREAD];
extern uint64_t try_read_op[MAX_APP_THREAD];
extern uint64_t read_leaf_retry[MAX_APP_THREAD];
extern uint64_t leaf_cache_invalid[MAX_APP_THREAD];
extern uint64_t try_speculative_read[MAX_APP_THREAD];
extern uint64_t correct_speculative_read[MAX_APP_THREAD];
extern uint64_t try_read_leaf[MAX_APP_THREAD];
extern uint64_t read_two_segments[MAX_APP_THREAD];
extern uint64_t try_read_hopscotch[MAX_APP_THREAD];
extern uint64_t retry_cnt[MAX_APP_THREAD][MAX_FLAG_NUM];
extern uint64_t leaf_read_sibling[MAX_APP_THREAD];

// Configuration
int kNodeCount = 2;
int kThreadCount = 1;
int kReadRatio = 70;
int kRangeRatio = 30;
uint64_t kTotalOps = 1000000;
int kRangeSize = 100;
double kZipfTheta = 0.99;          // Runtime parameter (matches DEX zipfian)
int kUniform = 0;                  // 0=Zipfian, 1=Uniform (matches DEX)
// Match DEX: kKeySpace = bulk_load_num + ceil((op_num+warmup_num)*(insertRatio/100.0)) + 1000
// With insertRatio=0: kKeySpace = bulk_load_count + 0 + 1000
uint64_t kKeySpace = 0;  // Calculated after parsing args

Tree *tree;
DSM *dsm;

// Operation type encoding (matches DEX)
enum op_type : uint8_t { Lookup = 0, Range = 1 };
uint64_t op_mask = (1ULL << 56) - 1;

// Pre-generated workload arrays (matches DEX approach)
uint64_t *warmup_array = nullptr;
uint64_t *workload_array = nullptr;
uint64_t warmup_per_thread = 0;
uint64_t ops_per_thread = 0;

/**
 * CityHash key derivation — MATCHES DEX to_key() exactly
 */
inline uint64_t to_key(uint64_t k) {
    return (CityHash64((char *)&k, sizeof(k)) + 1) % kKeySpace;
}

/**
 * Record latency in appropriate histogram bucket (no atomics)
 */
inline void record_latency(uint64_t* histogram, uint64_t ns) {
    uint64_t bucket = ns / LATENCY_NS_GRANULARITY;
    if (bucket >= LATENCY_BUCKETS) {
        bucket = LATENCY_BUCKETS - 1;
    }
    histogram[bucket]++;
}

/**
 * Pre-generate and shuffle workload arrays (matches DEX generate_workload())
 * Operation type is encoded in upper 8 bits of each entry.
 */
void generate_workload() {
    // Key generator setup — Zipfian or Uniform (matches DEX)
    struct zipf_gen_state gen_zipf;
    std::mt19937_64 uniform_rng(0xdeadbeef);
    std::uniform_int_distribution<uint64_t> uniform_dist(0, kKeySpace - 1);
    
    if (!kUniform) {
        mehcached_zipf_init(&gen_zipf, kKeySpace, kZipfTheta, 0xdeadbeef);
    }
    
    auto next_key = [&]() -> uint64_t {
        if (kUniform) {
            return to_key(uniform_dist(uniform_rng));
        } else {
            return to_key(mehcached_zipf_next(&gen_zipf));
        }
    };

    uint64_t total_warmup = warmup_per_thread * kThreadCount;
    uint64_t total_ops = ops_per_thread * kThreadCount;

    warmup_array = new uint64_t[total_warmup];
    workload_array = new uint64_t[total_ops];

    // Generate warmup array — FULL MIX (reads + ranges), not read-only
    for (uint64_t i = 0; i < total_warmup; i++) {
        uint64_t key = next_key();
        if ((i % 100) < (uint64_t)kReadRatio) {
            key |= (static_cast<uint64_t>(op_type::Lookup) << 56);
        } else {
            key |= (static_cast<uint64_t>(op_type::Range) << 56);
        }
        warmup_array[i] = key;
    }

    // Shuffle warmup array (matches DEX's shuffle with same seed)
    std::mt19937 gen(0xc70f6907UL);
    std::shuffle(&warmup_array[0], &warmup_array[total_warmup - 1], gen);

    // Generate workload array — FULL MIX
    for (uint64_t i = 0; i < total_ops; i++) {
        uint64_t key = next_key();
        if ((i % 100) < (uint64_t)kReadRatio) {
            key |= (static_cast<uint64_t>(op_type::Lookup) << 56);
        } else {
            key |= (static_cast<uint64_t>(op_type::Range) << 56);
        }
        workload_array[i] = key;
    }

    // Shuffle workload array
    std::mt19937 gen2(0xc70f6907UL);
    std::shuffle(&workload_array[0], &workload_array[total_ops - 1], gen2);

    printf("Workload generated: %lu warmup + %lu measurement ops (%s, theta=%.2f)\n", 
           total_warmup, total_ops, kUniform ? "Uniform" : "Zipfian", kZipfTheta);
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
    file << "# CHIME Latency Histogram - " << op_type << std::endl;
    file << "# Bucket size: " << LATENCY_NS_GRANULARITY << " ns" << std::endl;
    file << "# Total samples: " << total_samples << std::endl;
    file << "# Format: latency_ns\tcount" << std::endl;
    
    for (int b = 0; b < LATENCY_BUCKETS; b++) {
        if (total[b] > 0) {
            uint64_t latency_ns = (uint64_t)b * LATENCY_NS_GRANULARITY;
            file << latency_ns << "\t" << total[b] << std::endl;
        }
    }
    
    file.close();
    printf("Saved %s latency histogram to %s (%lu samples)\n", op_type, filename, total_samples);
    
    // Print percentiles
    printf("\n%s Latency Percentiles:\n", op_type);
    double percentiles[] = {50, 90, 95, 99, 99.9};
    uint64_t cumulative = 0;
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
    printf("Usage: %s <node_count> <thread_count> <read_ratio> <range_ratio> <total_ops> [range_size] [zipf_theta] [uniform]\n", prog);
    printf("\nExample for 2 nodes, 16 threads, 70%% reads, 30%% range scans, 5M ops, Zipfian 0.99:\n");
    printf("  %s 2 16 70 30 5000000 100 0.99 0\n", prog);
    printf("\nArguments:\n");
    printf("  node_count   - Total nodes (1 memory + N-1 compute)\n");
    printf("  thread_count - Threads per compute node\n");
    printf("  read_ratio   - Percentage of point reads (0-100)\n");
    printf("  range_ratio  - Percentage of range scans (0-100)\n");
    printf("  total_ops    - Total operations to run\n");
    printf("  range_size   - Count-based range scan size (default: 100)\n");
    printf("  zipf_theta   - Zipfian skew parameter (default: 0.99)\n");
    printf("  uniform      - 0=Zipfian, 1=Uniform (default: 0)\n");
    printf("  bulk_load_M  - Millions of keys to bulk load (default: 10)\n");
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
    if (argc > 7) kZipfTheta = atof(argv[7]);
    if (argc > 8) kUniform = atoi(argv[8]);
    if (argc > 9) bulk_load_count = atoll(argv[9]) * 1000000ULL;  // Convert M to actual count
    
    // Calculate kKeySpace after parsing bulk_load_count
    kKeySpace = bulk_load_count + KEY_SPACE_PADDING;
    
    if (kReadRatio + kRangeRatio != 100) {
        printf("Error: read_ratio + range_ratio must equal 100\n");
        return 1;
    }
    
    printf("===========================================\n");
    printf("CHIME Latency Benchmark (DEX-matched)\n");
    printf("===========================================\n");
    printf("Node count:     %d\n", kNodeCount);
    printf("Thread count:   %d\n", kThreadCount);
    printf("Read ratio:     %d%%\n", kReadRatio);
    printf("Range ratio:    %d%%\n", kRangeRatio);
    printf("Total ops:      %lu\n", kTotalOps);
    printf("Bulk load:      %lu keys (%lu M)\n", bulk_load_count, bulk_load_count / 1000000);
    printf("Key space:      %lu\n", kKeySpace);
    printf("Range size:     %d (count-based)\n", kRangeSize);
    printf("Latency bucket: %d ns\n", LATENCY_NS_GRANULARITY);
    printf("Zipf theta:     %.2f\n", kZipfTheta);
    printf("Uniform:        %s\n", kUniform ? "Yes" : "No (Zipfian)");
    printf("Key derivation: CityHash (matching DEX)\n");
    printf("Workload:       pre-generated + shuffled\n");
    printf("Warmup:         full-mix (reads + ranges)\n");
    printf("===========================================\n");
    
    // Initialize latency histograms and per-thread counters
    memset(read_latency, 0, sizeof(read_latency));
    memset(range_latency, 0, sizeof(range_latency));
    memset(thread_read_count, 0, sizeof(thread_read_count));
    memset(thread_range_count, 0, sizeof(thread_range_count));
    
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
        if (my_node == MEMORY_NODE_NUM) {
            printf("Node %d: Starting bulk load (%lu keys, kKeySpace=%lu)...\n", my_node, bulk_load_count, kKeySpace);
            
            for (uint64_t i = 0; i < bulk_load_count; i++) {
                Key k = int2key(i);
                tree->insert(k, i + 1);
                
                if ((i + 1) % 1000000 == 0) {
                    printf("  Loaded %lu / %lu keys\n", i + 1, bulk_load_count);
                }
            }
            printf("Node %d: Bulk load complete (%lu keys)\n", my_node, bulk_load_count);
        }
    }
    
    dsm->barrier("bulk_load");
    printf("Node %d: All nodes synchronized after bulk load\n", my_node);
    
    // Run benchmark on compute nodes with multiple threads
    if (my_node >= MEMORY_NODE_NUM) {
        // Pre-generate workload (like DEX)
        ops_per_thread = kTotalOps / kThreadCount;
        warmup_per_thread = WARMUP_OPS / kThreadCount;
        generate_workload();
        
        printf("Node %d: Starting benchmark (%lu ops, %d threads, range_size=%d)...\n", 
               my_node, kTotalOps, kThreadCount, kRangeSize);
        
        // Reset histograms before measurement
        memset(read_latency, 0, sizeof(read_latency));
        memset(range_latency, 0, sizeof(range_latency));
        memset(thread_read_count, 0, sizeof(thread_read_count));
        memset(thread_range_count, 0, sizeof(thread_range_count));
        memset(thread_throughput, 0, sizeof(thread_throughput));
        
        // Reset CHIME diagnostic counters
        tree->clear_debug_info();
        
        // Reset synchronization
        warmup_cnt.store(0);
        ready.store(false);
        
        // Launch worker threads
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreadCount; t++) {
            threads.emplace_back([t]() {
                bindCore(t);
                dsm->registerThread();
                
                // Each thread gets its slice of the pre-generated arrays
                uint64_t *my_warmup = warmup_array + t * warmup_per_thread;
                uint64_t *my_workload = workload_array + t * ops_per_thread;
                
                Timer timer;
                Value v;
                
                // ========== WARMUP — FULL MIX (reads + ranges) ==========
                for (uint64_t i = 0; i < warmup_per_thread; i++) {
                    uint64_t entry = my_warmup[i];
                    op_type cur_op = static_cast<op_type>(entry >> 56);
                    uint64_t key = entry & op_mask;
                    Key k = int2key(key);
                    
                    if (cur_op == op_type::Lookup) {
                        tree->search(k, v);
                    } else {
                        // Count-based range: scan kRangeSize consecutive keys
                        Key end_key = int2key(key + kRangeSize);
                        std::map<Key, Value> results;
                        tree->range_query(k, end_key, results);
                    }
                }
                
                // ========== BARRIER — sync all threads after warmup (DEX pattern) ==========
                warmup_cnt.fetch_add(1);
                if (t == 0) {
                    while (warmup_cnt.load() != kThreadCount);
                    printf("Node %d: All threads finished warmup\n", dsm->getMyNodeID());
                    // Reset diagnostic counters AFTER warmup
                    tree->clear_debug_info();
                    ready.store(true);
                }
                while (!ready.load());  // spin until all threads synced
                
                // ========== MEASUREMENT — with latency tracking ==========
                // Clear histograms for this thread before measurement
                memset(read_latency[t], 0, sizeof(uint64_t) * LATENCY_BUCKETS);
                memset(range_latency[t], 0, sizeof(uint64_t) * LATENCY_BUCKETS);
                
                // Per-thread timing (matches DEX approach — no warmup in throughput)
                auto thread_start = std::chrono::high_resolution_clock::now();
                
                for (uint64_t i = 0; i < ops_per_thread; i++) {
                    uint64_t entry = my_workload[i];
                    op_type cur_op = static_cast<op_type>(entry >> 56);
                    uint64_t key = entry & op_mask;
                    Key k = int2key(key);
                    
                    if (cur_op == op_type::Lookup) {
                        // Point read (lookup)
                        timer.begin();
                        tree->search(k, v);
                        uint64_t elapsed_ns = timer.end();
                        
                        record_latency(read_latency[t], elapsed_ns);
                        thread_read_count[t]++;
                    } else {
                        // Count-based range scan: from key to key+kRangeSize
                        Key end_key = int2key(key + kRangeSize);
                        std::map<Key, Value> results;
                        
                        timer.begin();
                        tree->range_query(k, end_key, results);
                        uint64_t elapsed_ns = timer.end();
                        
                        record_latency(range_latency[t], elapsed_ns);
                        thread_range_count[t]++;
                    }
                }
                
                auto thread_end = std::chrono::high_resolution_clock::now();
                auto thread_dur = std::chrono::duration_cast<std::chrono::microseconds>(thread_end - thread_start).count();
                if (thread_dur > 0) {
                    thread_throughput[t] = (uint64_t)(ops_per_thread / (thread_dur / 1e6));
                }
            });
        }
        
        // Wait for all threads to complete
        for (auto& t : threads) {
            t.join();
        }
        
        // Aggregate per-thread counters (no atomics needed — threads are joined)
        uint64_t total_reads_val = 0, total_ranges_val = 0;
        uint64_t total_throughput_val = 0;
        for (int t = 0; t < kThreadCount; t++) {
            total_reads_val += thread_read_count[t];
            total_ranges_val += thread_range_count[t];
            total_throughput_val += thread_throughput[t];
        }
        uint64_t total_completed = total_reads_val + total_ranges_val;
        
        // Aggregate CHIME diagnostic counters
        double total_cache_hit = 0, total_cache_miss = 0;
        uint64_t total_read_handover = 0, total_try_read = 0;
        uint64_t total_try_read_leaf = 0, total_read_leaf_retry = 0;
        uint64_t total_leaf_cache_invalid = 0, total_leaf_read_sibling = 0;
        uint64_t total_try_spec_read = 0, total_correct_spec_read = 0;
        uint64_t total_try_read_hopscotch = 0, total_read_two_segments = 0;
        uint64_t total_retry[MAX_FLAG_NUM] = {0};
        for (int t = 0; t < kThreadCount; t++) {
            total_cache_hit += cache_hit[t];
            total_cache_miss += cache_miss[t];
            total_read_handover += read_handover_num[t];
            total_try_read += try_read_op[t];
            total_try_read_leaf += try_read_leaf[t];
            total_read_leaf_retry += read_leaf_retry[t];
            total_leaf_cache_invalid += leaf_cache_invalid[t];
            total_leaf_read_sibling += leaf_read_sibling[t];
            total_try_spec_read += try_speculative_read[t];
            total_correct_spec_read += correct_speculative_read[t];
            total_try_read_hopscotch += try_read_hopscotch[t];
            total_read_two_segments += read_two_segments[t];
            for (int f = 0; f < MAX_FLAG_NUM; f++) total_retry[f] += retry_cnt[t][f];
        }
        
        printf("\n===========================================\n");
        printf("Benchmark Complete!\n");
        printf("===========================================\n");
        printf("Threads:        %d\n", kThreadCount);
        printf("Range size:     %d (count-based)\n", kRangeSize);
        printf("Total ops:      %lu\n", total_completed);
        printf("Total reads:    %lu\n", total_reads_val);
        printf("Total ranges:   %lu\n", total_ranges_val);
        printf("Throughput:     %.2f ops/sec  (sum of per-thread, excludes warmup)\n",
               (double)total_throughput_val);
        printf("===========================================\n");
        
        // CHIME diagnostic stats (like ycsb_test.cpp)
        printf("\n--- CHIME Diagnostic Stats ---\n");
        double total_cache_all = total_cache_hit + total_cache_miss;
        if (total_cache_all > 0)
            printf("Cache hit rate:            %.4f  (%.0f / %.0f)\n", total_cache_hit / total_cache_all, total_cache_hit, total_cache_all);
        if (total_try_read > 0)
            printf("Read delegation rate:      %.4f  (%lu / %lu)\n", (double)total_read_handover / total_try_read, total_read_handover, total_try_read);
        if (total_try_read_leaf > 0) {
            printf("Read leaf retry rate:      %.4f  (%lu / %lu)\n", (double)total_read_leaf_retry / total_try_read_leaf, total_read_leaf_retry, total_try_read_leaf);
            printf("Leaf cache invalid rate:   %.4f  (%lu / %lu)\n", (double)total_leaf_cache_invalid / total_try_read_leaf, total_leaf_cache_invalid, total_try_read_leaf);
            printf("Leaf read sibling rate:    %.4f  (%lu / %lu)\n", (double)total_leaf_read_sibling / total_try_read_leaf, total_leaf_read_sibling, total_try_read_leaf);
        }
        if (total_try_read_leaf > 0)
            printf("Speculative read rate:     %.4f  (%lu / %lu)\n", (double)total_try_spec_read / total_try_read_leaf, total_try_spec_read, total_try_read_leaf);
        if (total_try_spec_read > 0)
            printf("Correct speculative ratio: %.4f  (%lu / %lu)\n", (double)total_correct_spec_read / total_try_spec_read, total_correct_spec_read, total_try_spec_read);
        if (total_try_read_hopscotch > 0)
            printf("Read two hop-segments:     %.4f  (%lu / %lu)\n", (double)total_read_two_segments / total_try_read_hopscotch, total_read_two_segments, total_try_read_hopscotch);
        printf("Retry breakdown: first_try=%lu invalid_leaf=%lu invalid_node=%lu find_next=%lu\n",
               total_retry[0], total_retry[1], total_retry[2], total_retry[3]);
        tree->statistics();  // prints cache + idx_cache stats
        printf("------------------------------\n");
        
        // Save separate per-op latency histograms
        save_latency_histogram("chime_read_latency.dat", read_latency, kThreadCount, "Read");
        save_latency_histogram("chime_range_latency.dat", range_latency, kThreadCount, "Range");
        
        // Cleanup
        delete[] warmup_array;
        delete[] workload_array;
    }
    
    dsm->barrier("done");
    printf("Node %d: Benchmark finished\n", my_node);
    
    return 0;
}
