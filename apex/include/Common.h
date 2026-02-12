#ifndef __APEX_COMMON_H__
#define __APEX_COMMON_H__

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <queue>
#include <bitset>
#include <limits>
#include <array>

#include "Debug.h"
#include "HugePageAlloc.h"
#include "Rdma.h"
#include "WRLock.h"

// ─── Environment Config ────────────────────────────────────────────
#define MAX_MACHINE 20
#define MEMORY_NODE_NUM 1
#define CPU_PHYSICAL_CORE_NUM 72
#define MAX_CORO_NUM 8

#define LATENCY_WINDOWS 100000
#define PACKED_ADDR_ALIGN_BIT 8
#define CACHELINE_ALIGN_BIT 6
#define MAX_KEY_SPACE_SIZE 60000000
#define MESSAGE_SIZE 96
#define RAW_RECV_CQ_COUNT 4096
#define MAX_TREE_HEIGHT 20

// ─── Auxiliary Macros ──────────────────────────────────────────────
#define STRUCT_OFFSET(type, field)  ((char *)&((type *)(0))->field - (char *)((type *)(0)))
#define UNUSED(x) (void)(x)
#define ADD_ROUND(x, n) ((x) = ((x) + 1) % (n))
#define ROUND_UP(x, n) (((x) + (1<<(n)) - 1) & ~((1<<(n)) - 1))
#define ROUND_DOWN(x, n) ((x) & ~((1<<(n)) - 1))
#define ADD_CACHELINE_VERSION_SIZE(x, cvs) ((x) + ((x)/(64-(cvs)) + ((x)%(64-(cvs))?1:0))*(cvs))

// ─── App Thread Config ─────────────────────────────────────────────
#define MAX_APP_THREAD 36
#define APP_MESSAGE_NR 96
#define POLL_CQ_MAX_CNT_ONCE 8

// ─── Directory Thread Config ───────────────────────────────────────
#define NR_DIRECTORY 1
#define DIR_MESSAGE_NR 128

// On-chip memory detection
#ifndef ON_CHIP_SIZE
#define ON_CHIP_SIZE 256  // KB
#endif

void bindCore(uint16_t core);
char *getIP();
char *getMac();

inline int bits_in(std::uint64_t u) {
  auto bs = std::bitset<64>(u);
  return bs.count();
}

// ─── Coroutine Types ───────────────────────────────────────────────
#include <boost/coroutine2/all.hpp>
#include <boost/crc.hpp>

using CoroPush = boost::coroutines2::coroutine<int>::push_type;
using CoroPull = boost::coroutines2::coroutine<int>::pull_type;
using CoroQueue = std::queue<uint16_t>;

// ─── Key/Value Types ───────────────────────────────────────────────
using Key = uint64_t;
using Value = uint64_t;

namespace define {

constexpr uint64_t MB = 1024ull * 1024;
constexpr uint64_t GB = 1024ull * MB;
constexpr uint16_t kCacheLineSize = 64;

// ─── Remote Allocation ─────────────────────────────────────────
constexpr uint64_t dsmSize        = 8;          // GB
constexpr uint64_t kChunkSize     = 16 * MB;    // B

// ─── Local Allocation ──────────────────────────────────────────
constexpr uint64_t rdmaBufferSize = 1;          // GB

// ─── APEX Leaf Page ────────────────────────────────────────────
constexpr uint32_t kLeafPageSize    = 4096;     // 4 KB leaf page
constexpr uint32_t kMaxEntriesPerLeaf = 256;    // max KV entries per leaf
constexpr uint32_t kSuffixLen       = 4;        // bytes of suffix stored in leaf
constexpr uint32_t kEntrySize       = kSuffixLen + sizeof(Value) + sizeof(uint16_t);  // 14 bytes
constexpr uint32_t kSlotArraySize   = kMaxEntriesPerLeaf;  // 256 bytes (1 byte per slot)
constexpr uint32_t kLeafHeaderSize  = 32;       // version, count, split info, fences

// ─── APEX ASM ──────────────────────────────────────────────────
constexpr uint32_t kASMBucketsPerLeaf = 256;    // 1:1 with max entries
constexpr uint32_t kASMEntrySize      = 2;      // 1B hash + 1B position
constexpr uint32_t kMaxOverflowPerBucket = 4;   // max collision chain

// ─── APEX VE-ASM ───────────────────────────────────────────────
constexpr uint32_t kVEASMEntriesPerLeaf = 16;   // top-16 hot keys per leaf
constexpr uint32_t kVEASMEntrySize      = 24;   // suffix(4) + value(8) + version(8) + access_count(2) + flags(2)

// ─── APEX CPT (Compressed Prefix Trie) ─────────────────────────
constexpr uint32_t kMaxTrieDepth    = 8;        // max key length in bytes
constexpr uint32_t kTrieNodeSize    = 64;       // bytes per trie node (cacheline aligned)
constexpr uint32_t kMaxTrieFanout   = 256;      // byte range

// ─── Version Chain ─────────────────────────────────────────────
constexpr uint32_t kVersionChainCapacity = 64;  // entries per chain
constexpr uint32_t kVersionChainEntrySize = 24; // position(1) + pad(3) + value(8) + version(8) + timestamp(4)

// ─── Misc ──────────────────────────────────────────────────────
constexpr uint64_t kRootPointerStoreOffest = kChunkSize / 2;
constexpr uint64_t kKeyMin = 1;
constexpr uint64_t kLoadedKeyNum = 60000000;
constexpr Value kValueNull = std::numeric_limits<Value>::min();
constexpr Value kValueMin = 1;
constexpr Value kValueMax = std::numeric_limits<Value>::max();

// ─── RDMA Buffer Sizing ───────────────────────────────────────
constexpr int64_t  kPerThreadRdmaBuf = rdmaBufferSize * GB / MAX_APP_THREAD;
constexpr int64_t  kPerCoroRdmaBuf   = kPerThreadRdmaBuf / MAX_CORO_NUM;

// Allocation sizes for the infrastructure layer
constexpr uint32_t allocationLeafSize    = kLeafPageSize + 16;  // +16 for lock
constexpr uint32_t allocationInternalSize = kTrieNodeSize + 16;
constexpr uint32_t rdmaBufLeafSize       = kLeafPageSize + 16;

// Buffer partitions for RdmaBuffer
constexpr uint32_t bufferEntrySize    = 64;
constexpr uint32_t bufferMetadataSize = 64;
constexpr uint32_t bufferBlockSize    = 0;

// ─── On-chip Memory ───────────────────────────────────────────
constexpr uint64_t kLockStartAddr   = 0;
constexpr uint64_t kLockChipMemSize = ON_CHIP_SIZE * 1024;
constexpr uint64_t kLocalLockNum    = 4 * MB;
constexpr uint64_t kOnChipLockNum   = kLockChipMemSize * 8;

}  // namespace define


// ─── Timing Utilities ──────────────────────────────────────────────
static inline unsigned long long asm_rdtsc(void) {
  unsigned hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)lo) | (((unsigned long long)hi) << 32);
}

__inline__ unsigned long long rdtsc(void) {
  unsigned hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)lo) | (((unsigned long long)hi) << 32);
}

inline void mfence() { asm volatile("mfence" ::: "memory"); }
inline void compiler_barrier() { asm volatile("" ::: "memory"); }

#endif /* __APEX_COMMON_H__ */
