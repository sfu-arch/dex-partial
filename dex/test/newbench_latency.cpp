#include "Timer.h"
#include "../util/system.hpp"
#include "sherman_wrapper.h"
#include "smart/smart_wrapper.h"
#include "tree/leanstore_tree.h"
#include "uniform.h"
#include "uniform_generator.h"
#include "zipf.h"

#include <algorithm>
#include <city.h>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <numa.h>
#include <stdlib.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fstream>
#include <iomanip>
#include <atomic>

// Enable latency tracking
#define LATENCY_TRACKING 1
#define GLOBAL_WORKLOAD 1

// Latency buckets: 0-100us with 1ns granularity
#define LATENCY_BUCKETS 100000
#define LATENCY_NS_GRANULARITY 1  // 1 nanosecond per bucket

namespace sherman {
extern uint64_t cache_miss[MAX_APP_THREAD][8];
extern uint64_t cache_hit[MAX_APP_THREAD][8];
}

int kMaxThread = 32;
std::thread th[MAX_APP_THREAD];
uint64_t tp[MAX_APP_THREAD][8];
uint64_t total_tp[MAX_APP_THREAD];

// Latency histogram storage - per thread to avoid contention
uint64_t latency_histogram[MAX_APP_THREAD][LATENCY_BUCKETS];
std::atomic<uint64_t> total_latency_samples{0};

std::mutex mtx;
std::condition_variable cv;
uint32_t kReadRatio;
uint32_t kInsertRatio;
uint32_t kUpdateRatio;
uint32_t kDeleteRatio;
uint32_t kRangeRatio;
int kThreadCount;
int totalThreadCount;
int memThreadCount;
int kNodeCount;
int CNodeCount;
std::vector<Key> sharding;
uint64_t cache_mb;
uint64_t kKeySpace;
uint64_t threadKSpace;
uint64_t partition_space;
uint64_t left_bound = 0;
uint64_t right_bound = 0;
uint64_t op_num = 0;
uint64_t thread_op_num = 0;
uint64_t thread_warmup_num = 0;
uint64_t node_warmup_num = 0;
uint64_t node_op_num = 0;
uint64_t *bulk_array = nullptr;
uint64_t bulk_load_num = 0;
uint64_t warmup_num = 0;
int node_id = 0;
double zipfian;
uint64_t *insert_array = nullptr;
uint64_t insert_array_size = 0;
int tree_index = 0;
int check_correctness = 0;
int time_based = 1;
int early_stop = 0;  // Disable early stop for latency measurement
bool partitioned = false;
double rpc_rate = 0;
double admission_rate = 1;
struct zipf_gen_state state;
int uniform_workload = 0;
uniform_key_generator_t *uniform_generator = nullptr;

std::vector<double> admission_rate_vec = {1, 0.8, 0.4, 0.2, 0.1, 0.05, 0.01, 0};
std::vector<double> rpc_rate_vec = {1};
std::vector<uint64_t> throughput_vec;
std::vector<uint64_t> straggler_throughput_vec;

int auto_tune = 0;
int run_times = 1;
int cur_run = 0;

uint64_t *workload_array = nullptr;
uint64_t *warmup_array = nullptr;
enum op_type : uint8_t { Insert, Update, Lookup, Delete, Range };
uint64_t op_mask = (1ULL << 56) - 1;

tree_api<Key, Value> *tree;
DSM *dsm;

inline Key to_key(uint64_t k) {
  return (CityHash64((char *)&k, sizeof(k)) + 1) % kKeySpace;
}

inline Key to_partition_key(uint64_t k) {
  return (CityHash64((char *)&k, sizeof(k)) + 1) % partition_space;
}

std::atomic<int64_t> warmup_cnt{0};
std::atomic<uint64_t> worker{0};
std::atomic<uint64_t> execute_op{0};
std::atomic_bool ready{false};
std::atomic_bool one_finish{false};
std::atomic_bool ready_to_report{false};

// Record a latency sample (in nanoseconds)
inline void record_latency(int thread_id, uint64_t latency_ns) {
  uint64_t bucket = latency_ns / LATENCY_NS_GRANULARITY;
  if (bucket >= LATENCY_BUCKETS) {
    bucket = LATENCY_BUCKETS - 1;
  }
  latency_histogram[thread_id][bucket]++;
  total_latency_samples.fetch_add(1);
}

void reset_all_params() {
  warmup_cnt.store(0);
  worker.store(0);
  ready.store(false);
  one_finish.store(false);
  ready_to_report.store(false);
}

void thread_run(int id) {
  bindCore(id);
  dsm->registerThread();
  tp[id][0] = 0;
  total_tp[id] = 0;
  uint64_t my_id = kMaxThread * node_id + id;
  worker.fetch_add(1);
  printf("I am %lu\n", my_id);

  uint64_t *thread_workload_array = workload_array + id * thread_op_num;
  uint64_t *thread_warmup_array = warmup_array + id * thread_warmup_num;
  
  size_t counter = 0;
  size_t success_counter = 0;
  uint32_t scan_num = 100;
  std::pair<Key, Value> *result = new std::pair<Key, Value>[scan_num];

  // Warmup phase (no latency tracking)
  while (counter < thread_warmup_num) {
    uint64_t key = thread_warmup_array[counter];
    op_type cur_op = static_cast<op_type>(key >> 56);
    key = key & op_mask;
    switch (cur_op) {
    case op_type::Lookup: {
      Value v = key;
      auto flag = tree->lookup(key, v);
      if (flag) ++success_counter;
    } break;
    case op_type::Insert: {
      Value v = key + 1;
      auto flag = tree->insert(key, v);
      if (flag) ++success_counter;
    } break;
    case op_type::Update: {
      Value v = key;
      auto flag = tree->update(key, v);
      if (flag) ++success_counter;
    } break;
    case op_type::Delete: {
      auto flag = tree->remove(key);
      if (flag) ++success_counter;
    } break;
    case op_type::Range: {
      auto flag = tree->range_scan(key, scan_num, result);
      if (flag) ++success_counter;
    } break;
    default:
      std::cout << "OP Type NOT MATCH!" << std::endl;
    }
    ++counter;
  }

  warmup_cnt.fetch_add(1);
  if (id == 0) {
    std::cout << "Thread_op_num = " << thread_op_num << std::endl;
    while (warmup_cnt.load() != kThreadCount);
    printf("node %d finish warmup\n", dsm->getMyNodeID());
    tree->set_rpc_ratio(rpc_rate);
    std::cout << "RPC ratio = " << rpc_rate << std::endl;
    dsm->clear_rdma_statistic();
    tree->clear_statistic();
    dsm->barrier(std::string("warm_finish") + std::to_string(cur_run), CNodeCount);
    ready.store(true);
    warmup_cnt.store(0);
  }

  while (!ready_to_report.load());

  // Clear latency histogram for this thread before measurement
  memset(latency_histogram[id], 0, sizeof(uint64_t) * LATENCY_BUCKETS);

  // Main execution with latency tracking
  counter = 0;
  success_counter = 0;
  Timer op_timer;
  
  auto start = std::chrono::high_resolution_clock::now();
  while (counter < thread_op_num) {
    uint64_t key = thread_workload_array[counter];
    op_type cur_op = static_cast<op_type>(key >> 56);
    key = key & op_mask;
    
    op_timer.begin();  // Start timing
    
    switch (cur_op) {
    case op_type::Lookup: {
      Value v = key;
      auto flag = tree->lookup(key, v);
      if (flag) ++success_counter;
    } break;
    case op_type::Insert: {
      Value v = key + 1;
      auto flag = tree->insert(key, v);
      if (flag) ++success_counter;
    } break;
    case op_type::Update: {
      Value v = key;
      auto flag = tree->update(key, v);
      if (flag) ++success_counter;
    } break;
    case op_type::Delete: {
      auto flag = tree->remove(key);
      if (flag) ++success_counter;
    } break;
    case op_type::Range: {
      auto flag = tree->range_scan(key, scan_num, result);
      if (flag) ++success_counter;
    } break;
    default:
      std::cout << "OP Type NOT MATCH!" << std::endl;
    }
    
    uint64_t latency_ns = op_timer.end();  // Get latency in nanoseconds
    record_latency(id, latency_ns);
    
    tp[id][0]++;
    ++counter;
  }
  
  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  
  worker.fetch_sub(1);
  
  uint64_t throughput = counter / (static_cast<double>(duration) / std::pow(10, 6));
  total_tp[id] = throughput;
  execute_op.fetch_add(counter);
  
  delete[] result;
}

void parse_args(int argc, char *argv[]) {
  if (argc != 23) {
    printf("argc = %d\n", argc);
    printf("Usage: ./newbench_latency kNodeCount kReadRatio kInsertRatio kUpdateRatio "
           "kDeleteRatio kRangeRatio "
           "totalThreadCount memThreadCount "
           "cacheSize(MB) uniform_workload zipfian_theta bulk_load_num "
           "warmup_num op_num "
           "check_correctness(0=no, 1=yes) time_based(0=no, 1=yes) early_stop(0=no, 1=yes) "
           "index(0=cachepush, 1=sherman) rpc_rate admission_rate "
           "auto_tune(0=false, 1=true) kMaxThread\n");
    exit(-1);
  }

  kNodeCount = atoi(argv[1]);
  kReadRatio = atoi(argv[2]);
  kInsertRatio = atoi(argv[3]);
  kUpdateRatio = atoi(argv[4]);
  kDeleteRatio = atoi(argv[5]);
  kRangeRatio = atoi(argv[6]);
  assert((kReadRatio + kInsertRatio + kUpdateRatio + kDeleteRatio + kRangeRatio) == 100);

  totalThreadCount = atoi(argv[7]);
  memThreadCount = atoi(argv[8]);
  cache_mb = atoi(argv[9]);
  uniform_workload = atoi(argv[10]);
  zipfian = atof(argv[11]);
  bulk_load_num = atoi(argv[12]) * 1000 * 1000;
  warmup_num = atoi(argv[13]) * 1000 * 1000;
  op_num = atoi(argv[14]) * 1000 * 1000;
  check_correctness = atoi(argv[15]);
  time_based = atoi(argv[16]);
  early_stop = 0;  // Force disable for latency measurement

  thread_op_num = op_num / totalThreadCount;
  thread_warmup_num = warmup_num / totalThreadCount;

  tree_index = atoi(argv[18]);
  rpc_rate = atof(argv[19]);
  admission_rate = atof(argv[20]);
  auto_tune = atoi(argv[21]);
  kMaxThread = atoi(argv[22]);

  kKeySpace = bulk_load_num + ceil((op_num + warmup_num) * (kInsertRatio / 100.0)) + 1000;
  threadKSpace = kKeySpace / totalThreadCount;

  CNodeCount = (totalThreadCount % kMaxThread == 0)
                   ? (totalThreadCount / kMaxThread)
                   : (totalThreadCount / kMaxThread + 1);

  printf("DEX LATENCY BENCHMARK\n");
  printf("kNodeCount %d, kReadRatio %d, kInsertRatio %d, kUpdateRatio %d, "
         "kDeleteRatio %d, kRangeRatio %d\n",
         kNodeCount, kReadRatio, kInsertRatio, kUpdateRatio, kDeleteRatio, kRangeRatio);
  printf("totalThreadCount %d, memThreadCount %d, cache_size %lu MB\n",
         totalThreadCount, memThreadCount, cache_mb);
  printf("uniform_workload %u, zipfian %lf\n", uniform_workload, zipfian);
  printf("bulk_load_num %lu, warmup_num %lu, op_num %lu\n",
         bulk_load_num, warmup_num, op_num);
  printf("KeySpace = %lu\n", kKeySpace);
}

void bulk_load() {
  tree->bulk_load(bulk_array, bulk_load_num);
  if (partitioned && dsm->getMyNodeID() == 0) {
    assert(sharding.size() == (CNodeCount + 1));
    std::vector<Key> bound;
    for (int i = 0; i < CNodeCount - 1; ++i) {
      bound.push_back(sharding[i + 1]);
    }
    tree->set_shared(bound);
    tree->get_basic();
  }
  tree->set_bound(left_bound, right_bound);
  delete[] bulk_array;
  printf("node %d finish bulkload\n", dsm->getMyNodeID());
}

void generate_index() {
  numa_set_preferred(0);
  switch (tree_index) {
  case 0: // DEX
  {
    int cluster_num = CNodeCount;
    sharding.push_back(std::numeric_limits<Key>::min());
    for (int i = 0; i < cluster_num - 1; ++i) {
      sharding.push_back((threadKSpace * kMaxThread) + sharding[i]);
    }
    sharding.push_back(std::numeric_limits<Key>::max());
    tree = new cachepush::BTree<Key, Value>(
        dsm, 0, cache_mb, rpc_rate, admission_rate, sharding, cluster_num);
    partitioned = true;
  } break;
  case 1: // Sherman
  {
    tree = new sherman_wrapper<Key, Value>(dsm, 0, cache_mb);
    if (dsm->getMyNodeID() == 0) {
      for (uint64_t i = 1; i < 1024000; ++i) {
        tree->insert(to_key(i), i * 2);
      }
    }
  } break;
  case 2: // SMART
  {
    tree = new smart_wrapper<Key, Value>(dsm, 0, cache_mb);
  } break;
  }
  numa_set_localalloc();
}

void init_key_generator() {
  if (uniform_workload) {
    uniform_generator = new uniform_key_generator_t(kKeySpace);
  } else {
    mehcached_zipf_init(&state, kKeySpace, zipfian,
                        (rdtsc() & (0x0000ffffffffffffull)) ^ node_id);
  }
}

uint64_t generate_range_key() {
  uint64_t key = 0;
  while (true) {
    if (uniform_workload) {
      uint64_t dis = uniform_generator->next_id();
      key = to_key(dis);
    } else {
      uint64_t dis = mehcached_zipf_next(&state);
      key = to_key(dis);
    }
    if (key >= left_bound && key < right_bound) {
      break;
    }
  }
  return key;
}

void generate_workload() {
  uint64_t *space_array = new uint64_t[kKeySpace];
  for (uint64_t i = 0; i < kKeySpace; ++i) {
    space_array[i] = i;
  }
  bulk_array = new uint64_t[bulk_load_num];
  node_warmup_num = thread_warmup_num * kThreadCount;
  node_op_num = thread_op_num * kThreadCount;
  uint64_t warmup_insert_key_num = (kInsertRatio / 100.0) * node_warmup_num;
  uint64_t workload_insert_key_num = (kInsertRatio / 100.0) * node_op_num;
  uint64_t *insert_array = nullptr;

  if (partitioned) {
    left_bound = sharding[node_id];
    right_bound = sharding[node_id + 1];
    auto cluster_num = CNodeCount;
    if (node_id == (cluster_num - 1)) {
      right_bound = kKeySpace;
    }
    partition_space = right_bound - left_bound;

    uint64_t accumulated_bulk_num = 0;
    for (int i = 0; i < cluster_num; i++) {
      uint64_t left_b = sharding[i];
      uint64_t right_b = (i == (cluster_num - 1)) ? kKeySpace : sharding[i + 1];
      std::mt19937 gen(0xc70f6907UL);
      std::shuffle(&space_array[left_b], &space_array[right_b - 1], gen);
      uint64_t bulk_num_per_node =
          static_cast<uint64_t>(static_cast<double>(right_b - left_b + 1) /
                                kKeySpace * bulk_load_num);
      if (i == cluster_num - 1) {
        bulk_num_per_node = bulk_load_num - accumulated_bulk_num;
        assert(bulk_num_per_node <= (right_b - left_b + 1));
      } else {
        bulk_num_per_node = std::min<uint64_t>(bulk_num_per_node, right_b - left_b + 1);
      }
      memcpy(&bulk_array[accumulated_bulk_num], &space_array[left_b],
             sizeof(uint64_t) * bulk_num_per_node);
      accumulated_bulk_num += bulk_num_per_node;
      if (left_b == left_bound) {
        insert_array = space_array + left_b + bulk_num_per_node;
        assert((left_b + bulk_num_per_node + warmup_insert_key_num +
                workload_insert_key_num) <= right_b);
      }
    }
    assert(accumulated_bulk_num == bulk_load_num);
  } else {
    partition_space = kKeySpace;
    left_bound = 0;
    right_bound = kKeySpace;
    std::mt19937 gen(0xc70f6907UL);
    std::shuffle(&space_array[0], &space_array[kKeySpace - 1], gen);
    memcpy(&bulk_array[0], &space_array[0], sizeof(uint64_t) * bulk_load_num);

    uint64_t regular_node_insert_num =
        static_cast<uint64_t>(thread_warmup_num * kMaxThread * (kInsertRatio / 100.0)) +
        static_cast<uint64_t>(thread_op_num * kMaxThread * (kInsertRatio / 100.0));
    insert_array = space_array + bulk_load_num + regular_node_insert_num * node_id;
    assert((bulk_load_num + regular_node_insert_num * node_id +
            warmup_insert_key_num + workload_insert_key_num) <= kKeySpace);
  }

  init_key_generator();

  UniformRandom rng(rdtsc() ^ node_id);
  uint32_t random_num;
  auto insertmark = kReadRatio + kInsertRatio;
  auto updatemark = insertmark + kUpdateRatio;
  auto deletemark = updatemark + kDeleteRatio;
  auto rangemark = deletemark + kRangeRatio;

  warmup_array = new uint64_t[node_warmup_num];

  uint64_t i = 0;
  uint64_t insert_counter = 0;
  if (kInsertRatio == 100) {
    assert(uniform_workload == true);
    while (i < node_warmup_num) {
      uint64_t key = (insert_array[insert_counter] |
                      (static_cast<uint64_t>(op_type::Insert) << 56));
      warmup_array[i] = key;
      ++insert_counter;
      ++i;
    }
  } else {
    while (i < node_warmup_num) {
      random_num = rng.next_uint32() % 100;
      uint64_t key = generate_range_key();
      if (random_num < kReadRatio) {
        key = key | (static_cast<uint64_t>(op_type::Lookup) << 56);
      } else if (random_num < insertmark) {
        key = key | (static_cast<uint64_t>(op_type::Insert) << 56);
      } else if (random_num < updatemark) {
        key = key | (static_cast<uint64_t>(op_type::Update) << 56);
      } else if (random_num < deletemark) {
        key = key | (static_cast<uint64_t>(op_type::Delete) << 56);
      } else {
        key = key | (static_cast<uint64_t>(op_type::Range) << 56);
      }
      warmup_array[i] = key;
      ++i;
    }
  }

  std::mt19937 gen(0xc70f6907UL);
  std::shuffle(&warmup_array[0], &warmup_array[node_warmup_num - 1], gen);

  workload_array = new uint64_t[node_op_num];
  i = 0;
  insert_array = insert_array + insert_counter;
  insert_counter = 0;

  if (kInsertRatio == 100) {
    assert(uniform_workload == true);
    while (i < node_op_num) {
      uint64_t key = (insert_array[insert_counter] |
                      (static_cast<uint64_t>(op_type::Insert) << 56));
      workload_array[i] = key;
      ++insert_counter;
      ++i;
    }
  } else {
    while (i < node_op_num) {
      random_num = rng.next_uint32() % 100;
      uint64_t key = generate_range_key();
      if (random_num < kReadRatio) {
        key = key | (static_cast<uint64_t>(op_type::Lookup) << 56);
      } else if (random_num < insertmark) {
        key = key | (static_cast<uint64_t>(op_type::Insert) << 56);
      } else if (random_num < updatemark) {
        key = key | (static_cast<uint64_t>(op_type::Update) << 56);
      } else if (random_num < deletemark) {
        key = key | (static_cast<uint64_t>(op_type::Delete) << 56);
      } else {
        key = key | (static_cast<uint64_t>(op_type::Range) << 56);
      }
      workload_array[i] = key;
      ++i;
    }
  }

  delete[] space_array;
  std::cout << "Finish all workload generation" << std::endl;
}

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
  
  // Print statistics
  printf("\n========== DEX LATENCY STATISTICS ==========\n");
  printf("Total operations: %lu\n", total_ops);
  printf("Average latency: %.2f ns\n", avg_latency * LATENCY_NS_GRANULARITY);
  printf("P50 latency: %lu ns\n", p50 * LATENCY_NS_GRANULARITY);
  printf("P90 latency: %lu ns\n", p90 * LATENCY_NS_GRANULARITY);
  printf("P95 latency: %lu ns\n", p95 * LATENCY_NS_GRANULARITY);
  printf("P99 latency: %lu ns\n", p99 * LATENCY_NS_GRANULARITY);
  printf("P99.9 latency: %lu ns\n", p999 * LATENCY_NS_GRANULARITY);
  printf("=============================================\n\n");
  
  // Save to file
  std::ofstream out(filename);
  if (out.is_open()) {
    out << "# DEX Latency Histogram (nanoseconds)\n";
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

int main(int argc, char *argv[]) {
  bindCore(0);
  numa_set_preferred(0);
  parse_args(argc, argv);

  // Initialize latency histograms
  memset(latency_histogram, 0, sizeof(latency_histogram));

  DSMConfig config;
  config.machineNR = kNodeCount;
  config.memThreadCount = memThreadCount;
  config.computeNR = CNodeCount;
  config.index_type = tree_index;
  dsm = DSM::getInstance(config);
  cachepush::global_dsm_ = dsm;
  
  node_id = dsm->getMyNodeID();
  if (node_id == (CNodeCount - 1)) {
    kThreadCount = totalThreadCount - ((CNodeCount - 1) * kMaxThread);
  } else {
    kThreadCount = kMaxThread;
  }

  uint64_t total_cluster_tp = 0;
  
  if (node_id < CNodeCount) {
    dsm->registerThread();
    generate_index();
    
    dsm->barrier("bulkload", CNodeCount);
    dsm->resetThread();
    generate_workload();
    bulk_load();
    
    dsm->resetThread();
    dsm->registerThread();
    tree->reset_buffer_pool(true);
    dsm->barrier(std::string("benchmark") + std::to_string(cur_run), CNodeCount);
    tree->get_newest_root();
    tree->set_rpc_ratio(0);  // No RPC during warmup
    
    dsm->resetThread();
    reset_all_params();
    
    for (int i = 0; i < kThreadCount; i++) {
      th[i] = std::thread(thread_run, i);
    }
    
    // Wait for warmup
    while (!ready.load()) {
      sleep(1);
    }
    
    ready_to_report.store(true);
    
    // Wait for all threads to complete
    while (worker.load() != 0) {
      sleep(1);
    }
    
    for (int i = 0; i < kThreadCount; i++) {
      th[i].join();
    }
    
    uint64_t total_throughput = 0;
    for (int i = 0; i < kThreadCount; ++i) {
      total_throughput += total_tp[i];
    }
    
    total_cluster_tp = dsm->sum_total(total_throughput, CNodeCount, false);
    
    // Save latency histogram (only on node 0)
    if (node_id == 0) {
      save_latency_histogram("dex_latency.dat");
      printf("Final cluster throughput: %.3f Mops/s\n", total_cluster_tp / 1e6);
    }
  }

  dsm->barrier("finish");
  return 0;
}
