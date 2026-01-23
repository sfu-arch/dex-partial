#ifndef __HUGEPAGEALLOC_H__
#define __HUGEPAGEALLOC_H__

#include <cstdint>

#include <memory.h>
#include <numa.h>
#include <sys/mman.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

char *getIP();
inline void *hugePageAlloc(size_t size) {
  numa_set_preferred(0);
  
  // Try huge pages first
  void *res = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  
  if (res == MAP_FAILED) {
    // Fall back to regular pages
    printf("Huge pages failed, falling back to regular pages for %lu MB\n", size / (1024*1024));
    res = mmap(NULL, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (res == MAP_FAILED) {
      printf("mmap failed: errno=%d (%s)\n", errno, strerror(errno));
      return nullptr;
    }
  }
  
  numa_set_localalloc();
  return res;
}

#endif /* __HUGEPAGEALLOC_H__ */
