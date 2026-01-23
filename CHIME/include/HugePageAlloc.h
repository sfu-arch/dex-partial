#ifndef __HUGEPAGEALLOC_H__
#define __HUGEPAGEALLOC_H__


#include "Debug.h"

#include <cstdint>

#include <sys/mman.h>
#include <memory.h>
#include <numa.h>
#include <numaif.h>

#define NUMA_NODE 1   // [CONFIG] 1   (check from numastat)


char *getIP();
inline void *hugePageAlloc(size_t size) {
    numa_set_preferred(NUMA_NODE);
    
    // Try huge pages first
    void *res = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    
    if (res == MAP_FAILED) {
        // Fall back to regular pages if huge pages not available
        Debug::notifyInfo("Huge pages not available, falling back to regular pages for %lu MB", size / (1024*1024));
        res = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (res == MAP_FAILED) {
            Debug::notifyError("%s mmap failed (both huge and regular pages)!\n", getIP());
            return nullptr;
        }
        // Touch pages to ensure they're allocated
        memset(res, 0, size);
    }

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
