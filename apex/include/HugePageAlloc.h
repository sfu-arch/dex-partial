#ifndef __HUGEPAGEALLOC_H__
#define __HUGEPAGEALLOC_H__


#include "Debug.h"

#include <cstdint>

#include <sys/mman.h>
#include <sys/resource.h>
#include <memory.h>
#include <numa.h>
#include <numaif.h>
#include <errno.h>

// Auto-detect NUMA node or use node 0 as fallback
inline int get_numa_node() {
    int max_node = numa_max_node();
    if (max_node >= 1) {
        return 1;  // Prefer node 1 if available
    }
    return 0;  // Fall back to node 0
}

char *getIP();
inline void *hugePageAlloc(size_t size) {
    // Check memlock limit
    struct rlimit rl;
    getrlimit(RLIMIT_MEMLOCK, &rl);
    Debug::notifyInfo("Memlock limit: soft=%lu MB, hard=%lu MB, requested=%lu MB",
                      rl.rlim_cur / (1024*1024), rl.rlim_max / (1024*1024), size / (1024*1024));
    
    int numa_node = get_numa_node();
    numa_set_preferred(numa_node);
    Debug::notifyInfo("Using NUMA node %d (max available: %d)", numa_node, numa_max_node());
    
    // Try huge pages first
    void *res = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    
    if (res == MAP_FAILED) {
        // Fall back to regular pages if huge pages not available
        Debug::notifyInfo("Huge pages failed (errno=%d: %s), trying regular pages for %lu MB", 
                          errno, strerror(errno), size / (1024*1024));
        res = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (res == MAP_FAILED) {
            Debug::notifyError("%s mmap failed (errno=%d: %s)!\n", getIP(), errno, strerror(errno));
            return nullptr;
        }
        Debug::notifyInfo("Regular pages allocated at %p, size=%lu MB", res, size / (1024*1024));
    } else {
        Debug::notifyInfo("Huge pages allocated at %p, size=%lu MB", res, size / (1024*1024));
    }
    
    // Touch all pages to ensure they're faulted in
    memset(res, 0, size);

    return res;
}

inline void hugePageFree(void *addr, size_t size) {
    int res = munmap(addr, size);
    if (res == -1) {
        Debug::notifyError("%s munmap failed! %d\n", getIP(), errno);
    }
    return;
}

#endif /* __HUGEPAGEALLOC_H__ */
