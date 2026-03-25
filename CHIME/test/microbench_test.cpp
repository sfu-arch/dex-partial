#include "Tree.h"
#include "Timer.h"
#include "zipf.h"
#include "uniform.h"
#include "uniform_generator.h"

#include <city.h>
#include <stdlib.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <map>
#include <vector>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <random>

// Number of 1-second epochs to report throughput before stopping.
#define TEST_EPOCH 10
#define TIME_INTERVAL 1

// ── CHIME internal counters (defined in Tree.cpp / src/) ──────
extern double   cache_miss[MAX_APP_THREAD];
extern double   cache_hit[MAX_APP_THREAD];
extern uint64_t lock_fail[MAX_APP_THREAD];
extern uint64_t write_handover_num[MAX_APP_THREAD];
extern uint64_t try_write_op[MAX_APP_THREAD];
extern uint64_t read_handover_num[MAX_APP_THREAD];
extern uint64_t try_read_op[MAX_APP_THREAD];
extern uint64_t read_leaf_retry[MAX_APP_THREAD];
extern uint64_t leaf_cache_invalid[MAX_APP_THREAD];
extern uint64_t leaf_read_sibling[MAX_APP_THREAD];
extern uint64_t try_speculative_read[MAX_APP_THREAD];
extern uint64_t correct_speculative_read[MAX_APP_THREAD];
extern uint64_t try_read_leaf[MAX_APP_THREAD];
extern uint64_t read_two_segments[MAX_APP_THREAD];
extern uint64_t try_read_hopscotch[MAX_APP_THREAD];
extern uint64_t try_insert_op[MAX_APP_THREAD];
extern uint64_t split_node[MAX_APP_THREAD];
extern uint64_t try_write_segment[MAX_APP_THREAD];
extern uint64_t write_two_segments[MAX_APP_THREAD];
extern double   load_factor_sum[MAX_APP_THREAD];
extern uint64_t split_hopscotch[MAX_APP_THREAD];
extern uint64_t retry_cnt[MAX_APP_THREAD][MAX_FLAG_NUM];

// ── Benchmark parameters ──────────────────────────────────────
// kInsertRatio = 100 - kReadRatio - kRangeRatio
int      kReadRatio;
int      kRangeRatio;
int      kThreadCount;
int      kNodeCount;
int      kCoroCnt        = 2;
int      uniform_workload = 0;    // 0 = zipfian, 1 = uniform
double   zipf_theta       = 0.99;
uint64_t kBulkLoadNum     = 60000000ULL; // keys loaded at startup (default 60 M)
uint64_t kKeySpace;                       // set from kBulkLoadNum in parse_args
uint32_t kScanNum         = 100;          // keys per range scan (must be ≤255)

// ─────────────────────────────────────────────────────────────
std::thread th[MAX_APP_THREAD];
uint64_t    tp[MAX_APP_THREAD][MAX_CORO_NUM];

extern volatile bool need_stop;
extern volatile bool need_clear[MAX_APP_THREAD];
extern uint64_t latency[MAX_APP_THREAD][MAX_CORO_NUM][LATENCY_WINDOWS];
uint64_t latency_th_all[LATENCY_WINDOWS];

Tree *tree;
DSM  *dsm;

// Convert a raw uint64 into the CHIME Key type via CityHash so that
// the key distribution is spread across the key space.
inline Key to_key(uint64_t k) {
  return int2key(CityHash64((char *)&k, sizeof(k)) % kKeySpace);
}

// ── Per-thread request generator ─────────────────────────────
class RequsetGenBench : public RequstGen {
public:
  RequsetGenBench(DSM *dsm, int coro_id, int coro_cnt)
      : id(dsm->getMyThreadID()), coro_id(coro_id), coro_cnt(coro_cnt) {
    seed = (unsigned int)rdtsc();
    if (uniform_workload) {
      uniform_gen = new uniform_key_generator_t(kKeySpace);
    } else {
      mehcached_zipf_init(&zstate, kKeySpace, zipf_theta,
                          (rdtsc() & 0x0000ffffffffffffull) ^ (uint64_t)id);
    }
    // Each coroutine gets its own non-overlapping insert key range so that
    // concurrent inserts never collide.
    insert_key = kBulkLoadNum +
                 (uint64_t)kThreadCount * kCoroCnt * dsm->getMyNodeID() +
                 (uint64_t)id * kCoroCnt + coro_id;
  }

  ~RequsetGenBench() {
    delete uniform_gen;
  }

  Request next() override {
    Request  r;
    uint32_t rnd = rand_r(&seed) % 100;

    if (rnd < (uint32_t)kReadRatio) {
      r.req_type = SEARCH;
    } else if (rnd < (uint32_t)(kReadRatio + kRangeRatio)) {
      r.req_type   = SCAN;
      r.range_size = (uint8_t)std::min(kScanNum, 255u);
    } else {
      r.req_type = INSERT;
    }

    if (r.req_type == INSERT) {
      // Fresh key guaranteed to be outside the bulk-loaded set.
      r.k        = int2key(insert_key);
      insert_key += (uint64_t)kThreadCount * kCoroCnt * dsm->getClusterSize();
    } else {
      uint64_t dis = uniform_workload ? uniform_gen->next_id()
                                      : mehcached_zipf_next(&zstate);
      r.k = to_key(dis);
    }
    r.v = ++val;
    tp[id][coro_id]++;
    return r;
  }

private:
  int      id, coro_id, coro_cnt;
  uint64_t val = 0;
  unsigned int seed;
  struct zipf_gen_state   zstate {};
  uniform_key_generator_t *uniform_gen = nullptr;
  uint64_t insert_key;
};

RequstGen *gen_func(DSM *dsm, Request *req, int req_num,
                    int coro_id, int coro_cnt) {
  return new RequsetGenBench(dsm, coro_id, coro_cnt);
}

void work_func(Tree *tree, const Request &r, CoroPull *sink) {
  if (r.req_type == SEARCH) {
    Value v;
    tree->search(r.k, v, sink);
  } else if (r.req_type == INSERT) {
    tree->insert(r.k, r.v, sink);
  } else if (r.req_type == UPDATE) {
    tree->update(r.k, r.v, sink);
  } else {  // SCAN
    std::map<Key, Value> ret;
    tree->range_query(r.k, r.k + r.range_size, ret);
  }
}

// ── Bulk loader & benchmark thread ───────────────────────────
Timer                bench_timer;
std::atomic<int64_t> warmup_cnt{0};
std::atomic_bool     ready{false};

void thread_load(int id) {
  const int loader_num = std::min(kThreadCount, 8);
  uint64_t  all_loaders = (uint64_t)loader_num * dsm->getClusterSize();
  uint64_t  loader_id   = (uint64_t)loader_num * dsm->getMyNodeID() + id;

  printf("I am loader %lu\n", loader_id);
  for (uint64_t i = 0; i < kBulkLoadNum; ++i) {
    if (i % all_loaders == loader_id) {
      tree->insert(to_key(i), (Value)(i + 1));
    }
  }
  printf("loader %lu finish\n", loader_id);
}

void thread_run(int id) {
  bindCore(id * 2 + 1);
  dsm->registerThread();
  printf("I am worker %lu\n",
         (uint64_t)kThreadCount * dsm->getMyNodeID() + id);

  if (id == 0) bench_timer.begin();

  // Parallel bulk load using up to 8 threads.
  if (id < std::min(kThreadCount, 8))
    thread_load(id);

  warmup_cnt.fetch_add(1);

  if (id == 0) {
    while (warmup_cnt.load() != kThreadCount) ;
    printf("node %d finish loading\n", dsm->getMyNodeID());
    dsm->barrier("warm_finish");
    printf("load time %lds\n", bench_timer.end() / 1000 / 1000 / 1000);
    ready = true;
    warmup_cnt.store(-1);
  }
  while (warmup_cnt.load() != -1) ;

#ifdef USE_CORO
  tree->run_coroutine(gen_func, work_func, kCoroCnt);
#else
  Timer timer;
  auto  gen       = new RequsetGenBench(dsm, 0, 0);
  auto  thread_id = dsm->getMyThreadID();
  while (!need_stop) {
    auto r    = gen->next();
    timer.begin();
    work_func(tree, r, nullptr);
    auto us_10 = timer.end() / 100;
    if (us_10 >= LATENCY_WINDOWS) us_10 = LATENCY_WINDOWS - 1;
    latency[thread_id][0][us_10]++;
  }
  delete gen;
#endif
}

// ── Argument parsing ──────────────────────────────────────────
void parse_args(int argc, char *argv[]) {
  if (argc < 8 || argc > 10) {
    printf("Usage: ./microbench_test kNodeCount kReadRatio kRangeRatio "
           "kThreadCount uniform_workload zipf_theta kCoroCnt "
           "[bulk_M=60] [scan_num=100]\n"
           "  kInsertRatio = 100 - kReadRatio - kRangeRatio\n"
           "  uniform_workload: 1=uniform  0=zipfian\n");
    exit(-1);
  }
  kNodeCount       = atoi(argv[1]);
  kReadRatio       = atoi(argv[2]);
  kRangeRatio      = atoi(argv[3]);
  kThreadCount     = atoi(argv[4]);
  uniform_workload = atoi(argv[5]);
  zipf_theta       = atof(argv[6]);
  kCoroCnt         = atoi(argv[7]);
  if (argc >= 9)  kBulkLoadNum = (uint64_t)atoi(argv[8]) * 1000000ULL;
  if (argc >= 10) kScanNum     = (uint32_t)atoi(argv[9]);

  assert(kReadRatio + kRangeRatio <= 100);
  kKeySpace = kBulkLoadNum;

  printf("kNodeCount=%d  kReadRatio=%d  kRangeRatio=%d  kInsertRatio=%d\n"
         "kThreadCount=%d  uniform=%d  theta=%.3f  kCoroCnt=%d\n"
         "bulk_M=%lu  scan_num=%u\n",
         kNodeCount, kReadRatio, kRangeRatio, 100 - kReadRatio - kRangeRatio,
         kThreadCount, uniform_workload, zipf_theta, kCoroCnt,
         kBulkLoadNum / 1000000ULL, kScanNum);
}

// ── Latency snapshot ──────────────────────────────────────────
void save_latency(int epoch_id) {
  for (int i = 0; i < LATENCY_WINDOWS; ++i) {
    latency_th_all[i] = 0;
    for (int k = 0; k < MAX_APP_THREAD; ++k)
      for (int j = 0; j < MAX_CORO_NUM; ++j)
        latency_th_all[i] += latency[k][j][i];
  }
  std::ofstream fout("../us_lat/epoch_" + std::to_string(epoch_id) + ".lat");
  fout << std::setiosflags(std::ios::fixed) << std::setprecision(1);
  if (fout.is_open()) {
    for (int i = 0; i < LATENCY_WINDOWS; ++i)
      fout << i / 10.0 << "\t" << latency_th_all[i] << "\n";
    fout.close();
  }
  memset(latency, 0,
         sizeof(uint64_t) * MAX_APP_THREAD * MAX_CORO_NUM * LATENCY_WINDOWS);
}

// ── Main ──────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
  parse_args(argc, argv);

  DSMConfig config;
  assert(kNodeCount >= MEMORY_NODE_NUM);
  config.machineNR = kNodeCount;
  config.threadNR  = kThreadCount;
  dsm = DSM::getInstance(config);
  dsm->registerThread();
  bindCore(kThreadCount * 2 + 1);
  tree = new Tree(dsm);
  dsm->barrier("benchmark");

  memset(tp, 0, sizeof(tp));
  memset(latency, 0,
         sizeof(uint64_t) * MAX_APP_THREAD * MAX_CORO_NUM * LATENCY_WINDOWS);

  for (int i = 0; i < kThreadCount; i++)
    th[i] = std::thread(thread_run, i);

  while (!ready.load()) ;

  timespec s, e;
  uint64_t pre_tp = 0;
  int      count  = 0;

  clock_gettime(CLOCK_REALTIME, &s);
  while (!need_stop) {
    sleep(TIME_INTERVAL);
    clock_gettime(CLOCK_REALTIME, &e);
    int microseconds = (e.tv_sec - s.tv_sec) * 1000000 +
                       (double)(e.tv_nsec - s.tv_nsec) / 1000;

    uint64_t all_tp = 0;
    for (int i = 0; i < MAX_APP_THREAD; ++i)
      for (int j = 0; j < kCoroCnt; ++j)
        all_tp += tp[i][j];
    clock_gettime(CLOCK_REALTIME, &s);

    uint64_t cap = all_tp - pre_tp;
    pre_tp = all_tp;

    // ── Aggregate stats ──────────────────────────────────────
    double   all = 0, hit = 0;
    uint64_t lock_fail_cnt = 0, try_write_op_cnt = 0, write_handover_cnt = 0;
    uint64_t try_read_op_cnt = 0, read_handover_cnt = 0;
    uint64_t try_read_leaf_cnt = 0, read_leaf_retry_cnt = 0;
    uint64_t leaf_cache_invalid_cnt = 0;
    uint64_t try_speculative_read_cnt = 0, correct_speculative_read_cnt = 0;
    uint64_t try_read_hopscotch_cnt = 0, read_two_segments_cnt = 0;
    uint64_t try_insert_op_cnt = 0, split_node_cnt = 0;
    uint64_t try_write_segment_cnt = 0, write_two_segments_cnt = 0;
    double   load_factor_sum_all = 0, split_hopscotch_cnt = 0;

    for (int i = 0; i < MAX_APP_THREAD; ++i) {
      all                       += (cache_hit[i] + cache_miss[i]);
      hit                       += cache_hit[i];
      lock_fail_cnt             += lock_fail[i];
      try_write_op_cnt          += try_write_op[i];
      write_handover_cnt        += write_handover_num[i];
      try_read_op_cnt           += try_read_op[i];
      read_handover_cnt         += read_handover_num[i];
      try_read_leaf_cnt         += try_read_leaf[i];
      read_leaf_retry_cnt       += read_leaf_retry[i];
      leaf_cache_invalid_cnt    += leaf_cache_invalid[i];
      try_speculative_read_cnt  += try_speculative_read[i];
      correct_speculative_read_cnt += correct_speculative_read[i];
      try_read_hopscotch_cnt    += try_read_hopscotch[i];
      read_two_segments_cnt     += read_two_segments[i];
      try_insert_op_cnt         += try_insert_op[i];
      split_node_cnt            += split_node[i];
      try_write_segment_cnt     += try_write_segment[i];
      write_two_segments_cnt    += write_two_segments[i];
      load_factor_sum_all       += load_factor_sum[i];
      split_hopscotch_cnt       += split_hopscotch[i];
    }

    std::fill(need_clear, need_clear + MAX_APP_THREAD, true);
    save_latency(++count);
    if (count >= TEST_EPOCH) need_stop = true;

    double   per_node_tp = cap * 1.0 / microseconds;
    uint64_t cluster_tp  = dsm->sum((uint64_t)(per_node_tp * 1000));

    printf("%d, throughput %.4f Mops\n", dsm->getMyNodeID(), per_node_tp);

    if (dsm->getMyNodeID() == 0) {
      printf("epoch %d passed!\n", count);
      printf("cluster throughput %.3f Mops\n", cluster_tp / 1000.0);
      printf("cache hit rate: %.4lf\n", (all > 0) ? hit / all : 0.0);
      if (try_write_op_cnt > 0) {
        printf("avg. lock/cas fail cnt: %.4lf\n",
               lock_fail_cnt * 1.0 / try_write_op_cnt);
        printf("write combining rate: %.4lf\n",
               write_handover_cnt * 1.0 / try_write_op_cnt);
      }
      if (try_read_op_cnt > 0)
        printf("read delegation rate: %.4lf\n",
               read_handover_cnt * 1.0 / try_read_op_cnt);
      if (try_read_leaf_cnt > 0) {
        printf("read leaf retry rate: %.4lf\n",
               read_leaf_retry_cnt * 1.0 / try_read_leaf_cnt);
        printf("read invalid leaf rate: %.4lf\n",
               leaf_cache_invalid_cnt * 1.0 / try_read_leaf_cnt);
        printf("speculative read rate: %.4lf\n",
               try_speculative_read_cnt * 1.0 / try_read_leaf_cnt);
        if (try_speculative_read_cnt > 0)
          printf("correct speculative ratio: %.4lf\n",
                 correct_speculative_read_cnt * 1.0 / try_speculative_read_cnt);
      }
      if (try_read_hopscotch_cnt > 0)
        printf("read two hopscotch-segments rate: %.4lf\n",
               read_two_segments_cnt * 1.0 / try_read_hopscotch_cnt);
      if (try_write_segment_cnt > 0)
        printf("write two hopscotch-segments rate: %.4lf\n",
               write_two_segments_cnt * 1.0 / try_write_segment_cnt);
      if (try_insert_op_cnt > 0)
        printf("node split rate: %.4lf\n",
               split_node_cnt * 1.0 / try_insert_op_cnt);
      if (split_hopscotch_cnt > 0)
        printf("avg. leaf load factor: %.4lf\n",
               load_factor_sum_all / split_hopscotch_cnt);
      printf("\n");
    }
  }

  printf("[END]\n");
  for (int i = 0; i < kThreadCount; i++) {
    th[i].join();
    printf("Thread %d joined.\n", i);
  }
  tree->statistics();
  dsm->barrier("fin");
  return 0;
}
