#include "Directory.h"
#include "Common.h"

#include "Connection.h"
#include "cache/btree_rpc.h"

#include <gperftools/profiler.h>
#include <atomic>
#include <cstdio>

GlobalAddress g_root_ptr = GlobalAddress::Null();
int g_root_level = -1;
bool enable_cache;

// ── Per-directory RPC counters ────────────────────────────────────────────
// Indexed by dirID (max NR_DIRECTORY = 4).  Atomic so dirThread can update
// while the main thread (or another dir) reads for periodic printing.
static std::atomic<uint64_t> dir_lookup_cnt[NR_DIRECTORY];
static std::atomic<uint64_t> dir_update_cnt[NR_DIRECTORY];
static std::atomic<uint64_t> dir_insert_cnt[NR_DIRECTORY];
static std::atomic<uint64_t> dir_delete_cnt[NR_DIRECTORY];
static std::atomic<uint64_t> dir_malloc_cnt[NR_DIRECTORY];

// Print a summary line every this many RPC messages on a given dir thread.
static constexpr uint64_t kRpcPrintInterval = 1'000'000;

Directory::Directory(DirectoryConnection *dCon, RemoteConnection *remoteInfo,
                     uint32_t machineNR, uint16_t dirID, uint16_t nodeID,
                     int memThreadCount)
    : dCon(dCon), remoteInfo(remoteInfo), machineNR(machineNR), dirID(dirID),
      nodeID(nodeID), dirTh(nullptr) {

  { // chunck alloctor
    GlobalAddress dsm_start;
    uint64_t per_directory_dsm_size = dCon->dsmSize / memThreadCount;
    dsm_start.nodeID = nodeID;
    dsm_start.offset = per_directory_dsm_size * dirID;
    // std::cout << "Per directory DM size (MB) = "
    //           << per_directory_dsm_size / define::MB << std::endl;
    chunckAlloc = new GlobalAllocator(dsm_start, per_directory_dsm_size);
  }

  dirTh = new std::thread(&Directory::dirThread, this);
}

Directory::~Directory() { delete chunckAlloc; }

void Directory::dirThread() {
  // bindCore((19 - dirID) * 2);
  bindCore(39 - dirID);
  Debug::notifyInfo("dir %d launch!\n", dirID);

  // Zero our per-dir counters on start
  dir_lookup_cnt[dirID].store(0);
  dir_update_cnt[dirID].store(0);
  dir_insert_cnt[dirID].store(0);
  dir_delete_cnt[dirID].store(0);
  dir_malloc_cnt[dirID].store(0);

  uint64_t total_rpc = 0;

  while (true) {
    struct ibv_wc wc;
    pollWithCQ(dCon->cq, 1, &wc);
    switch (int(wc.opcode)) {
    case IBV_WC_RECV: // control message
    {
      auto *m = (RawMessage *)dCon->message->getMessage();
      process_message(m);
      ++total_rpc;

      // Periodic snapshot — emitted to stdout so it appears in the memory
      // node's log alongside the compute node output.
      if (total_rpc % kRpcPrintInterval == 0) {
        printf("[DIR%u] rpc_total=%lu  lookup=%lu  update=%lu"
               "  insert=%lu  delete=%lu  malloc=%lu\n",
               dirID, total_rpc,
               dir_lookup_cnt[dirID].load(),
               dir_update_cnt[dirID].load(),
               dir_insert_cnt[dirID].load(),
               dir_delete_cnt[dirID].load(),
               dir_malloc_cnt[dirID].load());
        fflush(stdout);
      }
      break;
    }
    case IBV_WC_RDMA_WRITE: {
      break;
    }
    case IBV_WC_RECV_RDMA_WITH_IMM: {
      break;
    }
    default:
      assert(false);
    }
  }
}

void Directory::process_message(const RawMessage *m) {
  RawMessage *send = nullptr;
  switch (m->type) {

  case RpcType::LOOKUP: {
    dir_lookup_cnt[dirID].fetch_add(1, std::memory_order_relaxed);
    auto addr = m->addr;
    Value v_result;
    GlobalAddress g_result;
    auto ret = cachepush::lookup(addr, remoteInfo[addr.nodeID].dsmBase, m->k,
                                 v_result, g_result);
    send = (RawMessage *)dCon->message->getSendPool();
    send->level = ret;
    if (ret == 1) {
      send->addr.val = v_result;
    } else if (ret == 2) {
      send->addr = g_result;
    }

    break;
  }

  case RpcType::UPDATE: {
    dir_update_cnt[dirID].fetch_add(1, std::memory_order_relaxed);
    auto addr = m->addr;
    auto ret =
        cachepush::update(addr, remoteInfo[addr.nodeID].dsmBase, m->k, m->v);
    send = (RawMessage *)dCon->message->getSendPool();
    send->level = ret;
    send->addr = addr;
    break;
  }

  case RpcType::INSERT: {
    dir_insert_cnt[dirID].fetch_add(1, std::memory_order_relaxed);
    auto addr = m->addr;
    auto ret =
        cachepush::insert(addr, remoteInfo[addr.nodeID].dsmBase, m->k, m->v);
    send = (RawMessage *)dCon->message->getSendPool();
    send->level = ret;
    send->addr = addr;
    break;
  }

  case RpcType::DELETE: {
    dir_delete_cnt[dirID].fetch_add(1, std::memory_order_relaxed);
    auto addr = m->addr;
    auto ret = cachepush::remove(addr, remoteInfo[addr.nodeID].dsmBase, m->k);
    send = (RawMessage *)dCon->message->getSendPool();
    send->level = ret;
    send->addr = addr;
    break;
  }

  case RpcType::MALLOC: {
    dir_malloc_cnt[dirID].fetch_add(1, std::memory_order_relaxed);
    send = (RawMessage *)dCon->message->getSendPool();
    send->addr = chunckAlloc->alloc_chunck();
    break;
  }

  case RpcType::NEW_ROOT: {

    if (g_root_level < m->level) {
      g_root_ptr = m->addr;
      g_root_level = m->level;
      if (g_root_level >= 3) {
        enable_cache = true;
      }
    }

    break;
  }

  default:
    assert(false);
  }

  if (send) {
    // printf("Send back the message to node %d, app %d\n", m->node_id,
    // m->app_id);
    dCon->sendMessage2App(send, m->node_id, m->app_id);
  }
}