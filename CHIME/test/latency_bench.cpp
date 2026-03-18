/**
 * latency_bench.cpp — CHIME Latency Benchmark
 *
 * Benchmarks the CHIME B+tree with Zipfian or uniform workloads,
 * measuring per-operation latency for reads and range scans.
 *
 * Usage (Node 0 = memory server, Node 1 = compute):
 *   ./latency_bench <node_count> <thread_count> <read_ratio> <range_ratio>
 *                  <total_ops> <range_size> <zipf_theta> <uniform> [bulk_load_M]
 *
 * Arguments:
 *   node_count   — total machines (memory + compute), e.g. 2
 *   thread_count — threads on this node
 *   read_ratio   — percentage of ops that are point reads (0-100)
 *   range_ratio  — percentage of ops that are range scans (0-100)
 *   total_ops    — total operations to run (absolute number)
 *   range_size   — keys per range scan
 *   zipf_theta   — Zipfian theta (ignored if uniform=1)
 *   uniform      — 1 = uniform distribution, 0 = Zipfian
 *   bulk_load_M  — millions of keys to pre-load (default: 10)
 *
 * Outputs (saved by the compute node):
 *   chime_read_latency.dat   — histogram (ns, count) for point reads
 *   chime_range_latency.dat  — histogram (ns, count) for range scans
 */

#include "Tree.h"
#include "DSM.h"
#include "Key.h"
#include "Timer.h"
#include "zipf.h"

#include <city.h>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdint.h>
#include <stdlib.h>
#include <thread>
#include <time.h>
#include <vector>

// ─── Latency histogram config ────────────────────────────────────────────────
// 500 ns buckets, 0–50 ms range = 100 000 buckets
#define LATENCY_BUCKETS     100000
#define LATENCY_NS_GRAN     500   // nanoseconds per bucket

// ─── Globals ─────────────────────────────────────────────────────────────────
int kNodeCount;
int kThreadCount;
uint32_t kReadRatio;     // 0-100
uint32_t kRangeRatio;    // 0-100
uint64_t kTotalOps;
int      kRangeSize;
double   kZipfTheta;
int      kUniform;
uint64_t kBulkLoadNum;   // absolute count
uint64_t kKeySpace;      // = kBulkLoadNum (key universe)

// Per-thread latency histograms (no atomics: aggregated after join)
uint64_t read_lat[MAX_APP_THREAD][LATENCY_BUCKETS];
uint64_t range_lat[MAX_APP_THREAD][LATENCY_BUCKETS];
uint64_t thread_read_cnt[MAX_APP_THREAD];
uint64_t thread_range_cnt[MAX_APP_THREAD];

// Throughput counters
uint64_t tp[MAX_APP_THREAD];

std::thread th[MAX_APP_THREAD];

extern volatile bool need_stop;
extern double cache_miss[MAX_APP_THREAD];
extern double cache_hit[MAX_APP_THREAD];

Tree *tree;
DSM  *dsm;

std::atomic<int64_t> warmup_cnt{0};
std::atomic_bool     ready{false};

// ─── Key derivation (same as DEX for fair comparison) ───────────────────────
inline Key to_key(uint64_t k) {
  uint64_t hashed = (CityHash64((char *)&k, sizeof(k)) + 1) % kKeySpace;
  return int2key(hashed);
}

// ─── Record a single latency sample (nanoseconds) ───────────────────────────
static inline void record_read(int tid, uint64_t ns) {
  uint64_t bucket = ns / LATENCY_NS_GRAN;
  if (bucket >= LATENCY_BUCKETS) bucket = LATENCY_BUCKETS - 1;
  read_lat[tid][bucket]++;
  thread_read_cnt[tid]++;
}
static inline void record_range(int tid, uint64_t ns) {
  uint64_t bucket = ns / LATENCY_NS_GRAN;
  if (bucket >= LATENCY_BUCKETS) bucket = LATENCY_BUCKETS - 1;
  range_lat[tid][bucket]++;
  thread_range_cnt[tid]++;
}

// ─── Timer helper ────────────────────────────────────────────────────────────
static inline uint64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// ─── Worker thread ───────────────────────────────────────────────────────────
void thread_run(int id) {
  bindCore(id * 2 + 1);
  dsm->registerThread();

  auto myNodeID = dsm->getMyNodeID();

  // Memory nodes just participate in barriers; compute nodes benchmark
  if (myNodeID < MEMORY_NODE_NUM) {
    // Wait until compute side is done
    dsm->barrier("warm_finish");
    dsm->barrier("run_finish");
    return;
  }

  // Compute node: bulk-load phase (only one designated loader per node)
  uint64_t loader_threads = std::min(kThreadCount, 8);
  if ((uint64_t)id < loader_threads) {
    uint64_t my_load_start = (kBulkLoadNum / loader_threads) * id;
    uint64_t my_load_end   = (id + 1 == (int)loader_threads)
                                 ? kBulkLoadNum
                                 : (kBulkLoadNum / loader_threads) * (id + 1);
    for (uint64_t i = my_load_start; i < my_load_end; ++i) {
      tree->insert(to_key(i), i + 1);
    }
  }

  warmup_cnt.fetch_add(1);
  if (id == 0) {
    while (warmup_cnt.load() != kThreadCount);
    printf("Node %d: bulk load complete (%lu keys)\n",
           (int)myNodeID, kBulkLoadNum);
    dsm->barrier("warm_finish");
    ready.store(true);
    warmup_cnt.store(-1);
  }
  while (warmup_cnt.load() != -1);

  // ── Benchmark phase ──────────────────────────────────────────────────────
  // Clear per-thread histograms
  memset(read_lat[id],  0, sizeof(uint64_t) * LATENCY_BUCKETS);
  memset(range_lat[id], 0, sizeof(uint64_t) * LATENCY_BUCKETS);
  thread_read_cnt[id]  = 0;
  thread_range_cnt[id] = 0;
  tp[id] = 0;

  uint64_t thread_ops   = kTotalOps / kThreadCount;
  uint64_t thread_start = (uint64_t)id * thread_ops;

  struct zipf_gen_state zstate;
  unsigned int seed = (unsigned int)(rdtsc() ^ ((uint64_t)id << 32));
  if (!kUniform) {
    mehcached_zipf_init(&zstate, kKeySpace, kZipfTheta,
                        (rdtsc() & 0x0000ffffffffffffull) ^ id);
  }

  for (uint64_t op = 0; op < thread_ops; ++op) {
    // Determine operation type
    uint32_t roll = rand_r(&seed) % 100;
    bool is_range = (roll < kRangeRatio);
    // read_ratio + range_ratio should <= 100; rest are updates (no-op here)

    // Key generation
    uint64_t raw;
    if (kUniform) {
      raw = (thread_start + op) % kKeySpace;
    } else {
      raw = mehcached_zipf_next(&zstate);
    }
    Key k = to_key(raw);

    if (is_range) {
      Key from = k;
      Key to   = to_key(raw + kRangeSize);
      if (to < from) to = from + kRangeSize;  // ensure from <= to

      std::map<Key, Value> ret;
      uint64_t t0 = now_ns();
      tree->range_query(from, to, ret);
      uint64_t t1 = now_ns();
      record_range(id, t1 - t0);
    } else {
      // Point read (covers reads and the remaining % as reads too)
      Value v;
      uint64_t t0 = now_ns();
      tree->search(k, v);
      uint64_t t1 = now_ns();
      record_read(id, t1 - t0);
    }
    tp[id]++;
  }

  dsm->barrier("run_finish");
}

// ─── Latency histogram save ───────────────────────────────────────────────────
void save_histogram(const std::string &filename,
                    uint64_t hist[][LATENCY_BUCKETS],
                    int nthreads, const char *op_type) {
  // Aggregate across threads
  uint64_t agg[LATENCY_BUCKETS] = {};
  for (int t = 0; t < nthreads; ++t)
    for (int i = 0; i < LATENCY_BUCKETS; ++i)
      agg[i] += hist[t][i];

  uint64_t total = 0;
  double   sum   = 0.0;
  for (int i = 0; i < LATENCY_BUCKETS; ++i) { total += agg[i]; sum += agg[i] * i; }

  if (total == 0) { printf("No %s samples.\n", op_type); return; }

  double avg = sum / total;

  // Percentiles
  uint64_t p50_t = total * 50 / 100, p95_t = total * 95 / 100,
           p99_t = total * 99 / 100, p999_t = total * 999 / 1000;
  uint64_t cum = 0, p50 = 0, p95 = 0, p99 = 0, p999 = 0;
  for (int i = 0; i < LATENCY_BUCKETS; ++i) {
    cum += agg[i];
    if (!p50  && cum >= p50_t)  p50  = i;
    if (!p95  && cum >= p95_t)  p95  = i;
    if (!p99  && cum >= p99_t)  p99  = i;
    if (!p999 && cum >= p999_t) p999 = i;
  }

  printf("\n===== CHIME %s LATENCY =====\n", op_type);
  printf("  Ops  : %lu\n", total);
  printf("  Avg  : %.1f ns\n", avg  * LATENCY_NS_GRAN);
  printf("  P50  : %lu ns\n",  p50  * LATENCY_NS_GRAN);
  printf("  P95  : %lu ns\n",  p95  * LATENCY_NS_GRAN);
  printf("  P99  : %lu ns\n",  p99  * LATENCY_NS_GRAN);
  printf("  P99.9: %lu ns\n",  p999 * LATENCY_NS_GRAN);
  printf("============================\n\n");

  std::ofstream out(filename);
  if (!out.is_open()) {
    printf("ERROR: cannot open %s\n", filename.c_str());
    return;
  }
  out << "# CHIME Latency Histogram - " << op_type << " (nanoseconds)\n";
  out << "# Total ops: " << total << "\n";
  out << "# Avg: "   << avg * LATENCY_NS_GRAN << " ns\n";
  out << "# P50: "   << p50  * LATENCY_NS_GRAN << " ns"
      << "  P95: "   << p95  * LATENCY_NS_GRAN << " ns"
      << "  P99: "   << p99  * LATENCY_NS_GRAN << " ns"
      << "  P99.9: " << p999 * LATENCY_NS_GRAN << " ns\n";
  out << "# latency_ns\tcount\n";
  for (int i = 0; i < LATENCY_BUCKETS; ++i)
    if (agg[i] > 0)
      out << (i * LATENCY_NS_GRAN) << "\t" << agg[i] << "\n";
  out.close();
  printf("Saved: %s\n", filename.c_str());
}

// ─── Argument parsing ────────────────────────────────────────────────────────
void parse_args(int argc, char *argv[]) {
  if (argc < 9 || argc > 10) {
    fprintf(stderr,
            "Usage: %s <node_count> <thread_count> <read_ratio> <range_ratio>"
            " <total_ops> <range_size> <zipf_theta> <uniform> [bulk_load_M]\n",
            argv[0]);
    exit(1);
  }
  kNodeCount   = atoi(argv[1]);
  kThreadCount = atoi(argv[2]);
  kReadRatio   = (uint32_t)atoi(argv[3]);
  kRangeRatio  = (uint32_t)atoi(argv[4]);
  kTotalOps    = (uint64_t)atoll(argv[5]);
  kRangeSize   = atoi(argv[6]);
  kZipfTheta   = atof(argv[7]);
  kUniform     = atoi(argv[8]);
  uint64_t bulk_M = (argc == 10) ? (uint64_t)atoi(argv[9]) : 10;
  kBulkLoadNum = bulk_M * 1000000ULL + 1000ULL;   // +1000 padding (same as DEX)
  kKeySpace    = kBulkLoadNum;

  printf("CHIME latency_bench config:\n");
  printf("  nodes=%d threads=%d read=%u%% range=%u%%\n",
         kNodeCount, kThreadCount, kReadRatio, kRangeRatio);
  printf("  total_ops=%lu range_size=%d theta=%.2f uniform=%d bulk_M=%lu\n",
         kTotalOps, kRangeSize, kZipfTheta, kUniform, bulk_M);
  printf("  key_space=%lu\n", kKeySpace);
}

// ─── Main ────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
  parse_args(argc, argv);

  DSMConfig config;
  assert(kNodeCount >= MEMORY_NODE_NUM);
  config.machineNR = (uint32_t)kNodeCount;
  config.threadNR  = (uint32_t)kThreadCount;

  dsm = DSM::getInstance(config);
  bindCore(kThreadCount * 2 + 1);
  dsm->registerThread();
  tree = new Tree(dsm, 0, (dsm->getMyNodeID() >= MEMORY_NODE_NUM));
  dsm->barrier("benchmark");

  // Zero out all histograms
  memset(read_lat,         0, sizeof(read_lat));
  memset(range_lat,        0, sizeof(range_lat));
  memset(thread_read_cnt,  0, sizeof(thread_read_cnt));
  memset(thread_range_cnt, 0, sizeof(thread_range_cnt));
  memset(tp, 0, sizeof(tp));

  for (int i = 0; i < kThreadCount; ++i)
    th[i] = std::thread(thread_run, i);

  // Wait for bulk-load / warm-up to finish
  while (!ready.load());

  // Measure throughput during run
  timespec ts_start, ts_end;
  uint64_t pre_tp = 0;
  clock_gettime(CLOCK_REALTIME, &ts_start);

  // Print throughput every second until workers finish
  for (int epoch = 0; epoch < 120; ++epoch) {
    sleep(1);
    uint64_t all_tp = 0;
    for (int i = 0; i < MAX_APP_THREAD; ++i) all_tp += tp[i];

    clock_gettime(CLOCK_REALTIME, &ts_end);
    int us = (ts_end.tv_sec - ts_start.tv_sec) * 1000000
           + (ts_end.tv_nsec - ts_start.tv_nsec) / 1000;
    clock_gettime(CLOCK_REALTIME, &ts_start);

    uint64_t cap = all_tp - pre_tp;
    pre_tp = all_tp;

    double hit = 0, miss = 0;
    for (int i = 0; i < MAX_APP_THREAD; ++i) {
      hit  += cache_hit[i];
      miss += cache_miss[i];
    }
    double hit_rate = (hit + miss > 0) ? hit / (hit + miss) : 0.0;

    printf("[epoch %3d] tput=%.2f Mops/s  cache_hit=%.4f\n",
           epoch,
           cap * 1.0 / us,
           hit_rate);

    // Check if all workers have completed (threads joined means run_finish barrier passed)
    uint64_t done = 0;
    for (int i = 0; i < kThreadCount; ++i) done += thread_read_cnt[i] + thread_range_cnt[i];
    if (done >= kTotalOps) break;
  }

  for (int i = 0; i < kThreadCount; ++i) th[i].join();

  // Only compute node saves results
  if (dsm->getMyNodeID() >= MEMORY_NODE_NUM) {
    uint64_t total_tp_cnt = 0;
    for (int i = 0; i < kThreadCount; ++i) total_tp_cnt += tp[i];
    printf("\nTotal throughput: %.2f Mops/s (%lu ops)\n",
           total_tp_cnt / 1e6, total_tp_cnt);

    save_histogram("chime_read_latency.dat",  read_lat,  kThreadCount, "Read");
    save_histogram("chime_range_latency.dat", range_lat, kThreadCount, "Range");
  }

  return 0;
}
