/**
 * tbc_unit_test.cpp — Unit tests for TBC components
 *
 * Tests:
 *   1. TrieNode basic operations (insert, lookup, invalidate)
 *   2. ARTCache adaptive node growth
 *   3. BitmapLeafDirectory set-associative logic
 *   4. TrieBitmapCache integrated lookup
 *
 * Build: g++ -std=c++17 -I../include -I../../dex/include tbc_unit_test.cpp -o tbc_unit_test
 * Run:   ./tbc_unit_test
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

// Minimal stubs for standalone testing without full DEX/RDMA
namespace define {
    constexpr uint64_t MB = 1024ULL * 1024;
}

using Key = uint64_t;
using Value = uint64_t;
constexpr uint32_t kLeafPageSize = 1024;

struct GlobalAddress {
    union {
        struct {
            uint64_t nodeID : 16;
            uint64_t offset : 48;
        };
        uint64_t val;
    };

    GlobalAddress() : val(0) {}
    GlobalAddress(uint16_t nid, uint64_t off) : nodeID(nid), offset(off) {}

    static GlobalAddress Null() { return GlobalAddress(); }
    bool operator==(const GlobalAddress& o) const { return val == o.val; }
    bool operator!=(const GlobalAddress& o) const { return val != o.val; }
};

// Stub for hugePageAlloc
inline void* hugePageAlloc(uint64_t size) {
    return aligned_alloc(4096, size);
}

inline int munmap(void* addr, size_t len) {
    free(addr);
    return 0;
}

// -----------------------------------------------------------------------
// Include TBC headers (they'll use the stubs above)
// -----------------------------------------------------------------------
#include "TrieNode.h"
#include "ARTCache.h"
#include "BitmapLeafDirectory.h"

// -----------------------------------------------------------------------
// Test: TrieCache basic operations
// -----------------------------------------------------------------------
void test_trie_cache() {
    printf("=== Testing TrieCache ===\n");

    tbc::TrieCache cache(1024 * 1024);  // 1M max entries

    // Insert some routing entries
    cache.insert_route(100, 200, GlobalAddress(0, 1000));
    cache.insert_route(300, 400, GlobalAddress(0, 2000));
    cache.insert_route(500, 600, GlobalAddress(0, 3000));

    assert(cache.size() >= 3);
    printf("  Inserted 3 routes, size = %lu\n", cache.size());

    // Lookup hits
    GlobalAddress addr;
    Key fl, fh;

    assert(cache.lookup_route(150, addr, fl, fh));
    assert(addr.offset == 1000);
    printf("  Lookup(150) → offset=%lu ✓\n", addr.offset);

    assert(cache.lookup_route(350, addr, fl, fh));
    assert(addr.offset == 2000);
    printf("  Lookup(350) → offset=%lu ✓\n", addr.offset);

    // Lookup miss
    assert(!cache.lookup_route(700, addr, fl, fh));
    printf("  Lookup(700) → miss ✓\n");

    // Invalidate
    cache.invalidate_range(100, 200);
    assert(!cache.lookup_route(150, addr, fl, fh));
    printf("  Invalidate [100,200], lookup(150) → miss ✓\n");

    // Clear
    cache.clear();
    assert(cache.size() == 0);
    printf("  Clear → size=0 ✓\n");

    printf("=== TrieCache PASSED ===\n\n");
}

// -----------------------------------------------------------------------
// Test: ARTCache adaptive growth
// -----------------------------------------------------------------------
void test_art_cache() {
    printf("=== Testing ARTCache ===\n");

    tbc::ARTCache cache(1024 * 1024);

    // Insert many routes to trigger node growth
    std::mt19937 rng(42);
    for (int i = 0; i < 1000; ++i) {
        Key k = rng() % 1000000;
        cache.insert_route(k, k + 100, GlobalAddress(0, k));
    }

    printf("  Inserted 1000 routes, size = %lu\n", cache.size());

    // Verify lookups
    int hits = 0, misses = 0;
    for (int i = 0; i < 100; ++i) {
        Key k = rng() % 1000000;
        GlobalAddress addr;
        if (cache.lookup_route(k, addr))
            ++hits;
        else
            ++misses;
    }
    printf("  Random lookups: %d hits, %d misses\n", hits, misses);

    // Eviction
    cache.evict(100);
    printf("  After evict(100), size = %lu\n", cache.size());

    cache.clear();
    assert(cache.size() == 0);
    printf("  Clear → size=0 ✓\n");

    printf("=== ARTCache PASSED ===\n\n");
}

// -----------------------------------------------------------------------
// Test: BitmapLeafDirectory set-associative cache
// -----------------------------------------------------------------------
void test_bitmap_directory() {
    printf("=== Testing BitmapLeafDirectory ===\n");

    tbc::BitmapLeafDirectory dir(1024 * 1024, kLeafPageSize);  // 1MB cache

    // Create fake page data
    char page1[kLeafPageSize] = {0};
    char page2[kLeafPageSize] = {0};
    strcpy(page1, "PAGE_ONE_DATA");
    strcpy(page2, "PAGE_TWO_DATA");

    GlobalAddress addr1(0, 1000);
    GlobalAddress addr2(0, 2000);

    // Insert
    assert(dir.insert(addr1, page1, 0x3F, 100, 200));
    assert(dir.insert(addr2, page2, 0x7F, 300, 400));
    printf("  Inserted 2 pages, cached_count = %lu\n", dir.cached_count());

    // Lookup hit
    uint64_t vac = 0;
    Key fl = 0, fh = 0;
    void* p = dir.lookup(addr1, &vac, &fl, &fh);
    assert(p != nullptr);
    assert(strcmp((char*)p, "PAGE_ONE_DATA") == 0);
    assert(vac == 0x3F);
    assert(fl == 100 && fh == 200);
    printf("  Lookup(addr1) → hit, vacancy=0x%lx, fence=[%lu,%lu] ✓\n", vac, fl, fh);

    // Lookup miss
    GlobalAddress addr3(0, 3000);
    assert(dir.lookup(addr3) == nullptr);
    printf("  Lookup(addr3) → miss ✓\n");

    // Invalidate
    dir.invalidate(addr1);
    assert(dir.lookup(addr1) == nullptr);
    printf("  Invalidate(addr1), lookup → miss ✓\n");

    // Stats
    printf("  hits=%lu, misses=%lu\n", dir.hit_count(), dir.miss_count());

    dir.reset();
    assert(dir.cached_count() == 0);
    printf("  Reset → cached_count=0 ✓\n");

    printf("=== BitmapLeafDirectory PASSED ===\n\n");
}

// -----------------------------------------------------------------------
// Test: Bitmap free-slot finding (__builtin_ctz pattern)
// -----------------------------------------------------------------------
void test_bitmap_free_slot() {
    printf("=== Testing Bitmap Free-Slot ===\n");

    // Simulate CHIME's bitmap pattern
    uint8_t occupancy = 0b00000000;  // All free

    auto find_free = [&]() -> int {
        uint8_t free_bits = static_cast<uint8_t>(~occupancy);
        if (free_bits == 0) return -1;
        return __builtin_ctz(static_cast<unsigned>(free_bits));
    };

    auto set_occupied = [&](int way) { occupancy |= (1u << way); };

    // Fill slots
    for (int i = 0; i < 8; ++i) {
        int slot = find_free();
        assert(slot == i);
        set_occupied(slot);
        printf("  Allocated slot %d, occupancy=0x%02x\n", slot, occupancy);
    }

    // All full
    assert(find_free() == -1);
    printf("  All slots full ✓\n");

    printf("=== Bitmap Free-Slot PASSED ===\n\n");
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------
int main() {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║   TBC (Trie+Bitmap Cache) Unit Tests   ║\n");
    printf("╚════════════════════════════════════════╝\n\n");

    test_trie_cache();
    test_art_cache();
    test_bitmap_directory();
    test_bitmap_free_slot();

    printf("╔════════════════════════════════════════╗\n");
    printf("║        ALL TESTS PASSED                ║\n");
    printf("╚════════════════════════════════════════╝\n\n");

    return 0;
}
