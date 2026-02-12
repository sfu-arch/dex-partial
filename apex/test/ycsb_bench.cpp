// ═══════════════════════════════════════════════════════════════════
//  APEX YCSB-style Benchmark (throughput focused)
// ═══════════════════════════════════════════════════════════════════
//
//  Usage: ./ycsb_bench <node_count> <thread_count> <read_ratio>
//                      <insert_ratio> <update_ratio> <total_ops>
//                      [zipf_theta]
//

#include "ApexIndex.h"
#include "Timer.h"
#include "DSM.h"
#include "zipf.h"

#include <city.h>
#include <thread>
#include <vector>
#include <iostream>
#include <fstream>
#include <random>
#include <atomic>
#include <algorithm>
#include <cstring>

#define BULK_LOAD_COUNT 10000000
#define WARMUP_OPS 1000000
#define KEY_SPACE_PADDING 1000000

uint64_t tp[MAX_APP_THREAD][8];
std::atomic<int64_t> warmup_cnt{0};
std::atomic<bool> ready{false};

int kNodeCount = 2;
int kThreadCount = 1;
int kReadRatio = 50;
int kInsertRatio = 25;
int kUpdateRatio = 25;
uint64_t kTotalOps = 10000000;
double kZipfTheta = 0.99;

apex::ApexIndex* tree;
DSM* dsm;

uint64_t* workload_array = nullptr;
uint64_t kKeySpace = BULK_LOAD_COUNT + KEY_SPACE_PADDING;

enum op_type : uint8_t { Read = 0, Insert = 1, Update = 2 };
uint64_t op_mask = (1ULL << 56) - 1;

inline uint64_t to_key(uint64_t raw) {
  return CityHash64((const char*)&raw, sizeof(raw));
}

void generate_workload(uint64_t count, double theta,
                       int read_pct, int insert_pct, int update_pct) {
  struct zipf_gen_state state;
  mehcached_zipf_init(&state, kKeySpace, theta, time(nullptr));

  for (uint64_t i = 0; i < count; i++) {
    int r = rand() % 100;
    uint64_t raw_key = mehcached_zipf_next(&state) + 1;

    if (r < read_pct) {
      workload_array[i] = ((uint64_t)Read << 56) | (raw_key & op_mask);
    } else if (r < read_pct + insert_pct) {
      // Insert uses keys beyond the bulk-loaded range
      raw_key = BULK_LOAD_COUNT + (raw_key % KEY_SPACE_PADDING) + 1;
      workload_array[i] = ((uint64_t)Insert << 56) | (raw_key & op_mask);
    } else {
      workload_array[i] = ((uint64_t)Update << 56) | (raw_key & op_mask);
    }
  }
}

void worker_thread(int thread_id, uint64_t ops_per_thread) {
  dsm->registerThread();
  bindCore(thread_id);

  // Warmup
  uint64_t warmup_share = WARMUP_OPS / kThreadCount;
  for (uint64_t i = 0; i < warmup_share; i++) {
    uint64_t key = to_key(rand() % BULK_LOAD_COUNT + 1);
    Value result;
    tree->lookup(key, &result);
  }

  warmup_cnt.fetch_add(warmup_share);
  while (warmup_cnt.load() < WARMUP_OPS) {
    asm volatile("pause");
  }

  tree->reset_stats();

  if (thread_id == 0) ready.store(true);
  while (!ready.load()) asm volatile("pause");

  // Timed phase
  Timer timer;
  timer.begin();

  uint64_t ops_start = thread_id * ops_per_thread;

  for (uint64_t i = 0; i < ops_per_thread; i++) {
    uint64_t encoded = workload_array[ops_start + i];
    uint8_t op = (uint8_t)(encoded >> 56);
    uint64_t raw = encoded & op_mask;
    uint64_t key = to_key(raw);

    switch (op) {
      case Read: {
        Value result;
        tree->lookup(key, &result);
        break;
      }
      case Insert: {
        tree->insert(key, raw);
        break;
      }
      case Update: {
        tree->update(key, raw + 1);
        break;
      }
    }
  }

  uint64_t total_ns = timer.end();
  double total_sec = total_ns / 1e9;
  tp[thread_id][0] = (uint64_t)(ops_per_thread / total_sec);
}

int main(int argc, char* argv[]) {
  if (argc < 7) {
    printf("Usage: %s <nodes> <threads> <read%%> <insert%%> <update%%> <total_ops> [zipf]\n", argv[0]);
    return 1;
  }

  kNodeCount = atoi(argv[1]);
  kThreadCount = atoi(argv[2]);
  kReadRatio = atoi(argv[3]);
  kInsertRatio = atoi(argv[4]);
  kUpdateRatio = atoi(argv[5]);
  kTotalOps = atol(argv[6]);
  if (argc > 7) kZipfTheta = atof(argv[7]);

  printf("═══════════════════════════════════════════════════\n");
  printf("  APEX YCSB Benchmark\n");
  printf("═══════════════════════════════════════════════════\n");
  printf("  Read: %d%%  Insert: %d%%  Update: %d%%\n", kReadRatio, kInsertRatio, kUpdateRatio);
  printf("  Threads: %d  Ops: %lu  Zipf: %.2f\n", kThreadCount, kTotalOps, kZipfTheta);
  printf("═══════════════════════════════════════════════════\n");

  DSMConfig config;
  config.machineNR = kNodeCount;
  config.threadNR = kThreadCount;
  config.dsmSize = define::dsmSize;
  config.cacheConfig.cacheSize = 72;
  dsm = DSM::getInstance(config);
  dsm->registerThread();

  tree = new apex::ApexIndex(dsm);

  // Bulk load
  printf("[APEX] Bulk loading %d keys...\n", BULK_LOAD_COUNT);
  uint64_t* bulk_keys = new uint64_t[BULK_LOAD_COUNT];
  uint64_t* bulk_vals = new uint64_t[BULK_LOAD_COUNT];
  struct KV { uint64_t key; uint64_t val; };
  auto kv_array = new KV[BULK_LOAD_COUNT];
  for (int i = 0; i < BULK_LOAD_COUNT; i++) {
    kv_array[i] = {to_key(i + 1), (uint64_t)(i + 1)};
  }
  std::sort(kv_array, kv_array + BULK_LOAD_COUNT,
            [](const KV& a, const KV& b) { return a.key < b.key; });
  for (int i = 0; i < BULK_LOAD_COUNT; i++) {
    bulk_keys[i] = kv_array[i].key;
    bulk_vals[i] = kv_array[i].val;
  }
  delete[] kv_array;

  tree->bulk_load(bulk_keys, bulk_vals, BULK_LOAD_COUNT);
  printf("[APEX] Bulk load complete: %u leaf pages\n", tree->get_trie().num_leaves());
  delete[] bulk_keys;
  delete[] bulk_vals;

  dsm->barrier("bulk_load_done");

  // Generate workload
  workload_array = new uint64_t[kTotalOps];
  generate_workload(kTotalOps, kZipfTheta, kReadRatio, kInsertRatio, kUpdateRatio);

  dsm->barrier("workload_ready");

  // Run benchmark
  uint64_t ops_per_thread = kTotalOps / kThreadCount;
  std::vector<std::thread> threads;
  for (int i = 1; i < kThreadCount; i++) {
    threads.emplace_back(worker_thread, i, ops_per_thread);
  }
  worker_thread(0, ops_per_thread);
  for (auto& t : threads) t.join();

  dsm->barrier("benchmark_done");

  // Results
  uint64_t total_tp = 0;
  for (int i = 0; i < kThreadCount; i++) {
    total_tp += tp[i][0];
  }

  printf("\n═══════════════════════════════════════════════════\n");
  printf("  APEX YCSB Results\n");
  printf("═══════════════════════════════════════════════════\n");
  printf("  Total throughput: %lu ops/sec (%.3f Mops)\n", total_tp, total_tp / 1e6);
  printf("═══════════════════════════════════════════════════\n\n");

  tree->print_stats();

  // Write to file
  char filename[256];
  snprintf(filename, sizeof(filename), "apex_ycsb_r%d_i%d_u%d_t%d_z%.2f.txt",
           kReadRatio, kInsertRatio, kUpdateRatio, kThreadCount, kZipfTheta);
  std::ofstream out(filename);
  out << "system=APEX\n";
  out << "threads=" << kThreadCount << "\n";
  out << "read_ratio=" << kReadRatio << "\n";
  out << "insert_ratio=" << kInsertRatio << "\n";
  out << "update_ratio=" << kUpdateRatio << "\n";
  out << "zipf_theta=" << kZipfTheta << "\n";
  out << "total_ops=" << kTotalOps << "\n";
  out << "throughput=" << total_tp << "\n";
  out.close();

  printf("[APEX] Results written to %s\n", filename);

  delete tree;
  delete[] workload_array;
  return 0;
}
