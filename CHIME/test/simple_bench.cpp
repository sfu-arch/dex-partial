/**
 * CHIME Simple Benchmark - matches ycsb_test structure
 * No YCSB files needed - generates keys directly
 */

#include "Tree.h"
#include "Timer.h"
#include <city.h>

#include <stdlib.h>
#include <thread>
#include <time.h>
#include <vector>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <random>
#include <algorithm>

#define TEST_EPOCH 10
#define TIME_INTERVAL 1.0
#define LOADER_NUM 8
#define LATENCY_BUCKETS 100000

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

int kNodeCount;
int kThreadCount;
uint64_t kKeyCount = 10000000;  // 10M keys
int kReadRatio = 100;

std::thread th[MAX_APP_THREAD];
uint64_t tp[MAX_APP_THREAD][8];
uint64_t latency_histogram[MAX_APP_THREAD][LATENCY_BUCKETS];

std::atomic<int64_t> warmup_cnt{0};
std::atomic<bool> ready{false};

Tree *tree;
DSM *dsm;

// Shuffled keys for loading
uint64_t *load_keys;

void thread_load(int id) {
    uint64_t per_thread = kKeyCount / LOADER_NUM;
    uint64_t start = id * per_thread;
    uint64_t end = (id == LOADER_NUM - 1) ? kKeyCount : start + per_thread;
    
    printf("Thread %d loading keys %lu to %lu\n", id, start, end);
    
    for (uint64_t i = start; i < end; ++i) {
        Key k = int2key(load_keys[i]);
        Value v = load_keys[i];
        tree->insert(k, v);
        
        if ((i - start) % 500000 == 0) {
            printf("Thread %d: loaded %lu keys\n", id, i - start);
        }
    }
}

void thread_run(int id) {
    bindCore(id * 2 + 1);
    dsm->registerThread();
    
    uint64_t my_id = kThreadCount * dsm->getMyNodeID() + id;
    printf("I am %lu\n", my_id);
    
    auto thread_id = dsm->getMyThreadID();
    
    // 1. Load phase (first LOADER_NUM threads)
    if (id < std::min(kThreadCount, LOADER_NUM)) {
        thread_load(id);
    }
    
    warmup_cnt.fetch_add(1);
    
    if (id == 0) {
        while (warmup_cnt.load() != kThreadCount);
        printf("node %d load finish\n", dsm->getMyNodeID());
        dsm->barrier("load_finish");
        ready.store(true);
        warmup_cnt.store(-1);
    }
    while (warmup_cnt.load() != -1);
    
    // 2. Benchmark phase - random reads
    std::mt19937_64 rng(my_id);
    std::uniform_int_distribution<uint64_t> dist(0, kKeyCount - 1);
    
    Timer timer;
    memset(latency_histogram[id], 0, sizeof(latency_histogram[id]));
    
    while (!need_stop) {
        uint64_t key_idx = dist(rng);
        Key k = int2key(load_keys[key_idx]);
        Value v;
        
        timer.begin();
        tree->search(k, v);
        uint64_t lat_ns = timer.end();
        
        // Record latency in microseconds
        uint64_t lat_us = lat_ns / 1000;
        if (lat_us >= LATENCY_BUCKETS) lat_us = LATENCY_BUCKETS - 1;
        latency_histogram[id][lat_us]++;
        
        tp[id][0]++;
    }
}

void save_latency_histogram() {
    uint64_t total[LATENCY_BUCKETS] = {0};
    uint64_t total_ops = 0;
    
    for (int t = 0; t < kThreadCount; ++t) {
        for (int i = 0; i < LATENCY_BUCKETS; ++i) {
            total[i] += latency_histogram[t][i];
            total_ops += latency_histogram[t][i];
        }
    }
    
    // Calculate percentiles
    uint64_t cumulative = 0;
    uint64_t p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0;
    double avg = 0;
    
    for (int i = 0; i < LATENCY_BUCKETS; ++i) {
        avg += total[i] * i;
        cumulative += total[i];
        if (p50 == 0 && cumulative >= total_ops * 0.50) p50 = i;
        if (p90 == 0 && cumulative >= total_ops * 0.90) p90 = i;
        if (p95 == 0 && cumulative >= total_ops * 0.95) p95 = i;
        if (p99 == 0 && cumulative >= total_ops * 0.99) p99 = i;
        if (p999 == 0 && cumulative >= total_ops * 0.999) p999 = i;
    }
    avg /= total_ops;
    
    printf("\n========== LATENCY STATISTICS ==========\n");
    printf("Total ops: %lu\n", total_ops);
    printf("Avg: %.2f us, P50: %lu us, P90: %lu us, P95: %lu us, P99: %lu us, P99.9: %lu us\n",
           avg, p50, p90, p95, p99, p999);
    printf("=========================================\n\n");
    
    std::ofstream out("chime_latency.dat");
    if (out.is_open()) {
        out << "# CHIME Latency Histogram\n";
        out << "# Total ops: " << total_ops << "\n";
        out << "# Avg: " << avg << " us\n";
        out << "# P50: " << p50 << " us, P90: " << p90 << " us, P95: " << p95 
            << " us, P99: " << p99 << " us, P99.9: " << p999 << " us\n";
        out << "# latency_us\tcount\n";
        for (int i = 0; i < LATENCY_BUCKETS; ++i) {
            if (total[i] > 0) {
                out << i << "\t" << total[i] << "\n";
            }
        }
        out.close();
        printf("Latency saved to chime_latency.dat\n");
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: ./simple_bench kNodeCount kThreadCount\n");
        printf("Example: ./simple_bench 2 16\n");
        exit(-1);
    }
    
    kNodeCount = atoi(argv[1]);
    kThreadCount = atoi(argv[2]);
    
    printf("kNodeCount=%d, kThreadCount=%d, kKeyCount=%lu\n", kNodeCount, kThreadCount, kKeyCount);
    
    // Generate shuffled load keys
    load_keys = new uint64_t[kKeyCount];
    for (uint64_t i = 0; i < kKeyCount; ++i) {
        load_keys[i] = i;
    }
    std::mt19937_64 shuffle_rng(0xDEADBEEF);
    std::shuffle(load_keys, load_keys + kKeyCount, shuffle_rng);
    
    // Initialize DSM
    DSMConfig config;
    config.machineNR = kNodeCount;
    config.threadNR = kThreadCount;
    dsm = DSM::getInstance(config);
    
    bindCore(kThreadCount * 2 + 1);
    dsm->registerThread();
    tree = new Tree(dsm);
    
    dsm->barrier("benchmark");
    
    // Launch threads
    for (int i = 0; i < kThreadCount; ++i) {
        th[i] = std::thread(thread_run, i);
    }
    
    // Wait for ready
    while (!ready.load());
    
    // Run for TEST_EPOCH intervals
    timespec s, e;
    uint64_t pre_tp = 0;
    int count = 0;
    
    clock_gettime(CLOCK_REALTIME, &s);
    while (!need_stop) {
        sleep(TIME_INTERVAL);
        clock_gettime(CLOCK_REALTIME, &e);
        
        int microseconds = (e.tv_sec - s.tv_sec) * 1000000 +
                          (double)(e.tv_nsec - s.tv_nsec) / 1000;
        
        uint64_t all_tp = 0;
        for (int i = 0; i < kThreadCount; ++i) {
            all_tp += tp[i][0];
        }
        clock_gettime(CLOCK_REALTIME, &s);
        
        uint64_t cap = all_tp - pre_tp;
        pre_tp = all_tp;
        
        double per_node_tp = cap * 1.0 / microseconds;
        uint64_t cluster_tp = dsm->sum((uint64_t)(per_node_tp * 1000));
        
        printf("%d, throughput %.4f Mops\n", dsm->getMyNodeID(), per_node_tp);
        
        if (dsm->getMyNodeID() == 0) {
            printf("epoch %d: cluster throughput %.3f Mops\n", count, cluster_tp / 1000.0);
        }
        
        if (++count >= TEST_EPOCH) {
            need_stop = true;
        }
    }
    
    // Join threads
    for (int i = 0; i < kThreadCount; ++i) {
        th[i].join();
        printf("Thread %d joined.\n", i);
    }
    
    // Save latency
    if (dsm->getMyNodeID() == 0) {
        save_latency_histogram();
    }
    
    tree->statistics();
    printf("[END]\n");
    dsm->barrier("fin");
    
    delete[] load_keys;
    return 0;
}
