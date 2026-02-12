#ifndef __APEX_CONFIG_H__
#define __APEX_CONFIG_H__

#include "Common.h"

class CacheConfig {
public:
  uint32_t cacheSize;  // GB for RDMA cache region

  CacheConfig(uint32_t cacheSize = define::rdmaBufferSize) : cacheSize(cacheSize) {}
};

class DSMConfig {
public:
  CacheConfig cacheConfig;
  uint32_t machineNR;
  uint32_t threadNR;
  uint64_t dsmSize;     // GB

  DSMConfig(const CacheConfig &cacheConfig = CacheConfig(),
            uint32_t machineNR = 2, uint64_t dsmSize = define::dsmSize)
      : cacheConfig(cacheConfig), machineNR(machineNR), dsmSize(dsmSize) {}
};

#endif /* __APEX_CONFIG_H__ */
