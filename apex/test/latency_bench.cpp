// ═══════════════════════════════════════════════════════════════════
//  APEX Latency Benchmark
// ═══════════════════════════════════════════════════════════════════
//
//  Usage: ./latency_bench <node_count> <thread_count> <read_ratio>
//                        <range_ratio> <total_ops> [range_size] [zipf_theta]
//
//  Matches the DEX/CHIME benchmark methodology for fair comparison:
//    - Same key derivation (CityHash)
//    - Same workload distribution (pre-generated arrays)
//    - Same latency histogram (500ns granularity)
//    - Same warmup phase (1M ops)
//    - Same bulk load (10M keys)
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
#include <chrono>
#include <map>

// ─── Configuration ─────────────────────────────────────────────────
#define LATENCY_NS_GRANULARITY 500
#define LATENCY_BUCKETS 100000
#define WARMUP_OPS 1000000
#define BULK_LOAD_COUNT 10000000
#define KEY_SPACE_PADDING 1000

// Per-thread latency histograms
uint64_t read_latency[MAX_APP_THREAD][LATENCY_BUCKETS];
uint64_t range_latency[MAX_APP_THREAD][LATENCY_BUCKETS];
uint64_t thread_read_count[MAX_APP_THREAD];
uint64_t thread_range_count[MAX_APP_THREAD];
uint64_t thread_throughput[MAX_APP_THREAD];

std::atomic<int64_t> warmup_cnt{0};
std::atomic<bool> ready{false};

// Parameters
int kNodeCount = 2;
int kThreadCount = 1;
int kReadRatio = 70;
int kRangeRatio = 30;
uint64_t kTotalOps = 1000000;
int kRangeSize = 100;
double kZipfTheta = 0.99;
uint64_t kKeySpace = BULK_LOAD_COUNT + KEY_SPACE_PADDING;

apex::ApexIndex* tree;
DSM* dsm;

// Workload arrays
uint64_t* warmup_array = nullptr;
uint64_t* workload_array = nullptr;

enum op_type : uint8_t { Lookup = 0, Range = 1 };
uint64_t op_mask = (1ULL << 56) - 1;

// ─── CityHash-based key derivation (matches DEX/CHIME) ────────────
inline uint64_t to_key(uint64_t raw) {
  return CityHash64((const char*)&raw, sizeof(raw));
}

// ─── Pre-generate workload ─────────────────────────────────────────
void generate_workload(uint64_t key_space, uint64_t count, double theta,
                       int read_ratio, int range_ratio, uint64_t* array) {
  struct zipf_gen_state state;
  mehcached_zipf_init(&state, key_space, theta, time(nullptr));

  for (uint64_t i = 0; i < count; i++) {
    int r = rand() % 100;
    uint64_t raw_key = mehcached_zipf_next(&state) + 1;

    if (r < read_ratio) {
      array[i] = ((uint64_t)Lookup << 56) | (raw_key & op_mask);
    } else {
      array[i] = ((uint64_t)Range << 56) | (raw_key & op_mask);
    }
  }
}

// ─── Worker Thread ─────────────────────────────────────────────────
void worker_thread(int thread_id, uint64_t ops_per_thread) {
  dsm->registerThread();
  bindCore(thread_id);

  memset(read_latency[thread_id], 0, sizeof(read_latency[thread_id]));
  memset(range_latency[thread_id], 0, sizeof(range_latency[thread_id]));
  thread_read_count[thread_id] = 0;
  thread_range_count[thread_id] = 0;

  // ── Warmup Phase ─────────────────────────────────────────────
  uint64_t warmup_share = WARMUP_OPS / kThreadCount;
  uint64_t warmup_start = thread_id * warmup_share;

  for (uint64_t i = 0; i < warmup_share; i++) {
    uint64_t raw = warmup_array[warmup_start + i] & op_mask;
    uint64_t key = to_key(raw);
    Value result;
    tree->lookup(key, &result);
    warmup_cnt.fetch_add(1);
  }

  // Wait for all threads to finish warmup
  while (warmup_cnt.load() < WARMUP_OPS) {
    asm volatile("pause");
  }

  // Reset stats after warmup
  tree->reset_stats();

  // Signal ready
  if (thread_id == 0) {
    ready.store(true);
  }
  while (!ready.load()) {
    asm volatile("pause");
  }

  // ── Timed Phase ──────────────────────────────────────────────
  Timer timer;
  timer.begin();

  uint64_t ops_start = thread_id * ops_per_thread;

  for (uint64_t i = 0; i < ops_per_thread; i++) {
    uint64_t encoded = workload_array[ops_start + i];
    uint8_t op = (uint8_t)(encoded >> 56);
    uint64_t raw = encoded & op_mask;
    uint64_t key = to_key(raw);

    Timer op_timer;
    op_timer.begin();

    if (op == Lookup) {
      Value result;
      tree->lookup(key, &result);

      uint64_t lat_ns = op_timer.end();
      uint64_t bucket = lat_ns / LATENCY_NS_GRANULARITY;
      if (bucket >= LATENCY_BUCKETS) bucket = LATENCY_BUCKETS - 1;
      read_latency[thread_id][bucket]++;
      thread_read_count[thread_id]++;
    } else {
      // Range scan
      std::pair<Key, Value> results[256];
      int n = tree->range_scan(key, kRangeSize, results);
      (void)n;

      uint64_t lat_ns = op_timer.end();
      uint64_t bucket = lat_ns / LATENCY_NS_GRANULARITY;
      if (bucket >= LATENCY_BUCKETS) bucket = LATENCY_BUCKETS - 1;
      range_latency[thread_id][bucket]++;
      thread_range_count[thread_id]++;
    }
  }

  uint64_t total_ns = timer.end();
  double total_sec = total_ns / 1e9;
  thread_throughput[thread_id] = (uint64_t)(ops_per_thread / total_sec);
}


// ─── Compute Percentile ────────────────────────────────────────────
double compute_percentile(uint64_t hist[][LATENCY_BUCKETS], int n_threads,
                          double percentile) {
  uint64_t merged[LATENCY_BUCKETS] = {0};
  uint64_t total = 0;
  for (int t = 0; t < n_threads; t++) {
    for (int b = 0; b < LATENCY_BUCKETS; b++) {
      merged[b] += hist[t][b];
      total += hist[t][b];
    }
  }

  uint64_t target = (uint64_t)(total * percentile);
  uint64_t cumulative = 0;
  for (int b = 0; b < LATENCY_BUCKETS; b++) {
    cumulative += merged[b];
    if (cumulative >= target) {
      return b * LATENCY_NS_GRANULARITY / 1000.0;  // return in microseconds
    }
  }
  return LATENCY_BUCKETS * LATENCY_NS_GRANULARITY / 1000.0;
}


// ─── Main ──────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
  if (argc < 6) {
    printf("Usage: %s <node_count> <thread_count> <read_ratio> <range_ratio> <total_ops> [range_size] [zipf_theta] [uniform]\n", argv[0]);
    return 1;
  }

  kNodeCount = atoi(argv[1]);
  kThreadCount = atoi(argv[2]);
  kReadRatio = atoi(argv[3]);
  kRangeRatio = atoi(argv[4]);
  kTotalOps = atol(argv[5]);
  if (argc > 6) kRangeSize = atoi(argv[6]);
  if (argc > 7) kZipfTheta = atof(argv[7]);
  int kUniform = 0;
  if (argc > 8) kUniform = atoi(argv[8]);
  if (kUniform) kZipfTheta = 0.0;  // uniform distribution

  printf("═══════════════════════════════════════════════════\n");
  printf("  APEX Latency Benchmark\n");
  printf("═══════════════════════════════════════════════════\n");
  printf("  Nodes:       %d\n", kNodeCount);
  printf("  Threads:     %d\n", kThreadCount);
  printf("  Read ratio:  %d%%\n", kReadRatio);
  printf("  Range ratio: %d%%\n", kRangeRatio);
  printf("  Total ops:   %lu\n", kTotalOps);
  printf("  Range size:  %d\n", kRangeSize);
  printf("  Zipf theta:  %.2f\n", kZipfTheta);
  printf("  Key space:   %lu\n", kKeySpace);
  printf("═══════════════════════════════════════════════════\n");

  // ── Initialize DSM ───────────────────────────────────────────
  DSMConfig config;
  config.machineNR = kNodeCount;
  config.threadNR = kThreadCount;
  config.dsmSize = define::dsmSize;
  config.cacheConfig.cacheSize = 72;  // 72 MB (CPT 15 + ASM 51 + VE-ASM 5 + VCS 1)
  dsm = DSM::getInstance(config);
  dsm->registerThread();

  // ── Bulk Load ────────────────────────────────────────────────
  printf("[APEX] Bulk loading %d keys...\n", BULK_LOAD_COUNT);

  tree = new apex::ApexIndex(dsm);

  // Generate sorted keys
  uint64_t* bulk_keys = new uint64_t[BULK_LOAD_COUNT];
  uint64_t* bulk_vals = new uint64_t[BULK_LOAD_COUNT];

  for (int i = 0; i < BULK_LOAD_COUNT; i++) {
    bulk_keys[i] = to_key(i + 1);
    bulk_vals[i] = i + 1;
  }

  // Sort by key for bulk load
  struct KV { uint64_t key; uint64_t val; };
  auto kv_array = new KV[BULK_LOAD_COUNT];
  for (int i = 0; i < BULK_LOAD_COUNT; i++) {
    kv_array[i] = {bulk_keys[i], bulk_vals[i]};
  }
  std::sort(kv_array, kv_array + BULK_LOAD_COUNT,
            [](const KV& a, const KV& b) { return a.key < b.key; });
  for (int i = 0; i < BULK_LOAD_COUNT; i++) {
    bulk_keys[i] = kv_array[i].key;
    bulk_vals[i] = kv_array[i].val;
  }
  delete[] kv_array;

  Timer load_timer;
  load_timer.begin();
  tree->bulk_load(bulk_keys, bulk_vals, BULK_LOAD_COUNT);
  uint64_t load_ns = load_timer.end();
  printf("[APEX] Bulk load complete: %.2f seconds, %u leaf pages\n",
         load_ns / 1e9, tree->get_trie().num_leaves());

  delete[] bulk_keys;
  delete[] bulk_vals;

  dsm->barrier("bulk_load_done");

  // ── Generate Workload ────────────────────────────────────────
  printf("[APEX] Generating workload (theta=%.2f)...\n", kZipfTheta);

  warmup_array = new uint64_t[WARMUP_OPS];
  workload_array = new uint64_t[kTotalOps];

  generate_workload(kKeySpace, WARMUP_OPS, kZipfTheta, 100, 0, warmup_array);
  generate_workload(kKeySpace, kTotalOps, kZipfTheta, kReadRatio, kRangeRatio, workload_array);

  printf("[APEX] Workload generated.\n");

  dsm->barrier("workload_ready");

  // ── Launch Worker Threads ────────────────────────────────────
  uint64_t ops_per_thread = kTotalOps / kThreadCount;
  std::vector<std::thread> threads;

  for (int i = 1; i < kThreadCount; i++) {
    threads.emplace_back(worker_thread, i, ops_per_thread);
  }
  worker_thread(0, ops_per_thread);

  for (auto& t : threads) t.join();

  dsm->barrier("benchmark_done");

  // ── Aggregate Results ────────────────────────────────────────
  uint64_t total_throughput = 0;
  uint64_t total_reads = 0, total_ranges = 0;
  for (int i = 0; i < kThreadCount; i++) {
    total_throughput += thread_throughput[i];
    total_reads += thread_read_count[i];
    total_ranges += thread_range_count[i];
  }

  double read_p50 = compute_percentile(read_latency, kThreadCount, 0.50);
  double read_p99 = compute_percentile(read_latency, kThreadCount, 0.99);
  double read_p999 = compute_percentile(read_latency, kThreadCount, 0.999);
  double range_p50 = compute_percentile(range_latency, kThreadCount, 0.50);
  double range_p99 = compute_percentile(range_latency, kThreadCount, 0.99);

  printf("\n═══════════════════════════════════════════════════\n");
  printf("  APEX Results\n");
  printf("═══════════════════════════════════════════════════\n");
  printf("  Throughput:     %lu ops/sec (%.3f Mops)\n", total_throughput, total_throughput / 1e6);
  printf("  Read count:     %lu\n", total_reads);
  printf("  Range count:    %lu\n", total_ranges);
  printf("  Read  P50:      %.1f us\n", read_p50);
  printf("  Read  P99:      %.1f us\n", read_p99);
  printf("  Read  P99.9:    %.1f us\n", read_p999);
  printf("  Range P50:      %.1f us\n", range_p50);
  printf("  Range P99:      %.1f us\n", range_p99);
  printf("═══════════════════════════════════════════════════\n\n");

  // Print APEX internal stats
  tree->print_stats();

  // ── Write results to summary file ──────────────────────────────
  char filename[256];
  snprintf(filename, sizeof(filename), "apex_results_t%d_z%.2f.txt",
           kThreadCount, kZipfTheta);
  std::ofstream out(filename);
  out << "system=APEX" << std::endl;
  out << "threads=" << kThreadCount << std::endl;
  out << "total_ops=" << kTotalOps << std::endl;
  out << "zipf_theta=" << kZipfTheta << std::endl;
  out << "read_ratio=" << kReadRatio << std::endl;
  out << "range_ratio=" << kRangeRatio << std::endl;
  out << "throughput=" << total_throughput << std::endl;
  out << "read_p50_us=" << read_p50 << std::endl;
  out << "read_p99_us=" << read_p99 << std::endl;
  out << "read_p999_us=" << read_p999 << std::endl;
  out << "range_p50_us=" << range_p50 << std::endl;
  out << "range_p99_us=" << range_p99 << std::endl;
  out << "read_count=" << total_reads << std::endl;
  out << "range_count=" << total_ranges << std::endl;

  // Write full histograms (CSV format)
  out << "\n# Read latency histogram (bucket_us, count)" << std::endl;
  for (int b = 0; b < LATENCY_BUCKETS; b++) {
    uint64_t total = 0;
    for (int t = 0; t < kThreadCount; t++) total += read_latency[t][b];
    if (total > 0) {
      out << (b * LATENCY_NS_GRANULARITY / 1000.0) << "," << total << std::endl;
    }
  }

  out << "\n# Range latency histogram (bucket_us, count)" << std::endl;
  for (int b = 0; b < LATENCY_BUCKETS; b++) {
    uint64_t total = 0;
    for (int t = 0; t < kThreadCount; t++) total += range_latency[t][b];
    if (total > 0) {
      out << (b * LATENCY_NS_GRANULARITY / 1000.0) << "," << total << std::endl;
    }
  }

  out.close();
  printf("[APEX] Summary written to %s\n", filename);

  // ── Write .dat files (CHIME-compatible format) ───────────────
  // Read latency histogram
  {
    std::ofstream dat("apex_read_latency.dat");
    dat << "# APEX Latency Histogram - Read" << std::endl;
    dat << "# Bucket size: " << LATENCY_NS_GRANULARITY << " ns" << std::endl;
    dat << "# Total samples: " << total_reads << std::endl;
    dat << "# Format: latency_ns\tcount" << std::endl;
    for (int b = 0; b < LATENCY_BUCKETS; b++) {
      uint64_t cnt = 0;
      for (int t = 0; t < kThreadCount; t++) cnt += read_latency[t][b];
      if (cnt > 0) {
        dat << (b * LATENCY_NS_GRANULARITY) << "\t" << cnt << std::endl;
      }
    }
    dat.close();
    printf("[APEX] Read latency histogram: apex_read_latency.dat\n");
  }

  // Range latency histogram
  {
    std::ofstream dat("apex_range_latency.dat");
    dat << "# APEX Latency Histogram - Range" << std::endl;
    dat << "# Bucket size: " << LATENCY_NS_GRANULARITY << " ns" << std::endl;
    dat << "# Total samples: " << total_ranges << std::endl;
    dat << "# Format: latency_ns\tcount" << std::endl;
    for (int b = 0; b < LATENCY_BUCKETS; b++) {
      uint64_t cnt = 0;
      for (int t = 0; t < kThreadCount; t++) cnt += range_latency[t][b];
      if (cnt > 0) {
        dat << (b * LATENCY_NS_GRANULARITY) << "\t" << cnt << std::endl;
      }
    }
    dat.close();
    printf("[APEX] Range latency histogram: apex_range_latency.dat\n");
  }

  delete tree;
  delete[] warmup_array;
  delete[] workload_array;

  return 0;
}
