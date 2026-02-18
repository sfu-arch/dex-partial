cmake_minimum_required(VERSION 3.10.2 FATAL_ERROR)
project(CHIME)

#Compiler options
# set(CMAKE_C_FLAGS "-Wall -Wno-deprecated-declarations -Wsign-compare -g")   # -DNDEBUG
set(CMAKE_C_FLAGS "-Wall -Wno-deprecated-declarations -Wno-unused-variable -Wno-unused-but-set-variable -Wsign-compare -O3")  # -O3
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++17")

# ============================================================================
# RDMA Driver Detection
# ============================================================================
# Detect if MLNX_OFED experimental verbs are available
include(CheckIncludeFileCXX)
check_include_file_cxx("infiniband/verbs_exp.h" HAS_VERBS_EXP)

option(FORCE_EXP_VERBS "Force use of experimental verbs (requires MLNX_OFED)" OFF)
option(FORCE_STD_VERBS "Force use of standard verbs only (RC transport)" OFF)

if(FORCE_EXP_VERBS)
    message(STATUS "RDMA: Forcing experimental verbs (MLNX_OFED required)")
    add_definitions(-DFORCE_USE_EXP_VERBS)
elseif(FORCE_STD_VERBS)
    message(STATUS "RDMA: Forcing standard verbs (RC transport, no device memory)")
    add_definitions(-DFORCE_NO_EXP_VERBS)
elseif(HAS_VERBS_EXP)
    message(STATUS "RDMA: Found experimental verbs (verbs_exp.h) - using DC transport")
    add_definitions(-DFORCE_USE_EXP_VERBS)
else()
    message(STATUS "RDMA: Experimental verbs not found - using RC transport fallback")
    add_definitions(-DFORCE_NO_EXP_VERBS)
endif()

# Print RDMA configuration summary
message(STATUS "")
message(STATUS "=== RDMA Configuration ===")
if(FORCE_EXP_VERBS OR (HAS_VERBS_EXP AND NOT FORCE_STD_VERBS))
    message(STATUS "  Transport:      DC (Dynamically Connected)")
    message(STATUS "  Device Memory:  Enabled")
    message(STATUS "  Masked Atomics: Enabled")
else()
    message(STATUS "  Transport:      RC (Reliably Connected) - FALLBACK")
    message(STATUS "  Device Memory:  Emulated with host memory")
    message(STATUS "  Masked Atomics: Standard atomics only")
endif()
message(STATUS "==========================")

#Link Options
set(LINKS_FLAGS "-lnuma -lcityhash -lboost_coroutine -lboost_context -lpthread -libverbs -lmemcached -ltbb")

#Env Options
option (STATIC_MN_IP "Use static MNs according the IPs of the nodes" OFF)
option (ENABLE_CORO "Turn on the coroutine" ON)
option (ENABLE_CACHE "Turn on the computing-side cache" ON)
option (SHORT_TEST_EPOCH "Use small epoch num and short epoch duration" OFF)
option (MIDDLE_TEST_EPOCH "Use middle epoch num and short epoch duration" OFF)
option (LONG_TEST_EPOCH "Use big epoch num and long epoch duration" OFF)
option (ENABLE_CACHE_EVICTION "Turn on cache-eviction" OFF)
option (READ_DELEGATION "Turn on read delegation technique" ON)
option (WRITE_COMBINING "Turn on write combining technique" ON)

if(STATIC_MN_IP)
    add_definitions(-DSTATIC_ID_FROM_IP)
else()
    remove_definitions(-DSTATIC_ID_FROM_IP)
endif()

if(ENABLE_CORO)
    add_definitions(-DUSE_CORO)
else()
    remove_definitions(-DUSE_CORO)
endif()

if(ENABLE_CACHE)
    add_definitions(-DTREE_ENABLE_CACHE)
else()
    remove_definitions(-DTREE_ENABLE_CACHE)
endif()

if(SHORT_TEST_EPOCH)
    add_definitions(-DSHORT_TEST_EPOCH)
else()
    remove_definitions(-DSHORT_TEST_EPOCH)
endif()

if(MIDDLE_TEST_EPOCH)
    add_definitions(-DMIDDLE_TEST_EPOCH)
else()
    remove_definitions(-DMIDDLE_TEST_EPOCH)
endif()

if(LONG_TEST_EPOCH)
    add_definitions(-DLONG_TEST_EPOCH)
else()
    remove_definitions(-DLONG_TEST_EPOCH)
endif()

if(ENABLE_CACHE_EVICTION)
    add_definitions(-DENABLE_CACHE_EVICTION)
else()
    remove_definitions(-DENABLE_CACHE_EVICTION)
endif()

if(READ_DELEGATION)
    add_definitions(-DTREE_ENABLE_READ_DELEGATION)
else()
    remove_definitions(-DTREE_ENABLE_READ_DELEGATION)
endif()

if(WRITE_COMBINING)
    add_definitions(-DTREE_ENABLE_WRITE_COMBINING)
else()
    remove_definitions(-DTREE_ENABLE_WRITE_COMBINING)
endif()

#Tree Options (compile into CHIME/baselines; these options should be set up one after one; CHIME is the B+ tree that turns on all options)
option (HOPSCOTCH_LEAF_NODE "+ Hopscotch leaf node" ON)
option (VACANCY_AWARE_LOCK "+ Vacancy bitmap piggybacking" ON)
option (METADATA_REPLICATION "+ Leaf metadata replication" ON)
option (SIBLING_BASED_VALIDATION "+ Sibling-based validation" ON)
option (SPECULATIVE_READ "+ Speculative read" ON)
# Variable-length KV
option (ENABLE_VAR_LEN_KV "Turn on the support for variable-length KVs" OFF)

if(HOPSCOTCH_LEAF_NODE)
    add_definitions(-DHOPSCOTCH_LEAF_NODE)
else()
    remove_definitions(-DHOPSCOTCH_LEAF_NODE)
endif()

if(VACANCY_AWARE_LOCK)
    add_definitions(-DVACANCY_AWARE_LOCK)
else()
    remove_definitions(-DVACANCY_AWARE_LOCK)
endif()

if(METADATA_REPLICATION)
    add_definitions(-DMETADATA_REPLICATION)
else()
    remove_definitions(-DMETADATA_REPLICATION)
endif()

if(SIBLING_BASED_VALIDATION)
    add_definitions(-DSIBLING_BASED_VALIDATION)
else()
    remove_definitions(-DSIBLING_BASED_VALIDATION)
endif()

if(SPECULATIVE_READ)
    add_definitions(-DSPECULATIVE_READ)
else()
    remove_definitions(-DSPECULATIVE_READ)
endif()

if(ENABLE_VAR_LEN_KV)
    add_definitions(-DENABLE_VAR_LEN_KV)
else()
    remove_definitions(-DENABLE_VAR_LEN_KV)
endif()

#Coherent Tree Options (NEW - replaces SkipList with Trie cache + compute-side bitmap)
option (USE_COHERENT_TREE "Use CoherentTree with trie cache and compute-side bitmap" OFF)
option (USE_TRIE_CACHE "Use trie-based cache instead of SkipList (requires USE_COHERENT_TREE)" ON)
option (USE_BITMAP_CACHE "Use compute-side bitmap cache (requires USE_COHERENT_TREE)" ON)
option (USE_LAZY_COHERENCE "Use lazy (version-based) coherence instead of eager invalidation" ON)

if(USE_COHERENT_TREE)
    add_definitions(-DUSE_COHERENT_TREE)
    if(USE_TRIE_CACHE)
        add_definitions(-DUSE_TRIE_CACHE)
    endif()
    if(USE_BITMAP_CACHE)
        add_definitions(-DUSE_BITMAP_CACHE)
    endif()
    if(USE_LAZY_COHERENCE)
        add_definitions(-DUSE_LAZY_COHERENCE)
    endif()
endif()

#Other Options
option (CACHE_MORE_INTERNAL_NODE "Cache higher-level internal nodes" ON)
option (UNORDERED_INTERNAL_NODE "Use KV-unordered internal nodes" OFF)
option (SPLIT_WRITE_UNLATCH "Write back split node and unlock with one WRITE" ON)
# Range-query-related options
option (FINE_GRAINED_RANGE_QUERY "+ Fine-grained range query" ON)
option (GREEDY_RANGE_QUERY "+ Greedy range query" ON)

if(CACHE_MORE_INTERNAL_NODE)
    add_definitions(-DCACHE_MORE_INTERNAL_NODE)
else()
    remove_definitions(-DCACHE_MORE_INTERNAL_NODE)
endif()

if(UNORDERED_INTERNAL_NODE)
    add_definitions(-DUNORDERED_INTERNAL_NODE)
else()
    remove_definitions(-DUNORDERED_INTERNAL_NODE)
endif()

if(SPLIT_WRITE_UNLATCH)
    add_definitions(-DSPLIT_WRITE_UNLATCH)
else()
    remove_definitions(-DSPLIT_WRITE_UNLATCH)
endif()

if(FINE_GRAINED_RANGE_QUERY)
    add_definitions(-DFINE_GRAINED_RANGE_QUERY)
else()
    remove_definitions(-DFINE_GRAINED_RANGE_QUERY)
endif()

if(GREEDY_RANGE_QUERY)
    add_definitions(-DGREEDY_RANGE_QUERY)
else()
    remove_definitions(-DGREEDY_RANGE_QUERY)
endif()

#Include files
set(INCLUDE_BASE ${PROJECT_SOURCE_DIR}/include)
include_directories(${INCLUDE_BASE})

#Source file define
set(COMMON_SRC ${PROJECT_SOURCE_DIR}/src)

#Used by both server and clients
file(GLOB_RECURSE COMMON_FILE ${COMMON_SRC}/*.cpp)
add_library(CHIME STATIC ${COMMON_FILE})
link_libraries(CHIME)

#Test codes
file(GLOB TEST_SRC ${PROJECT_SOURCE_DIR}/test/*.cpp)
foreach (TEST ${TEST_SRC})
    get_filename_component(TEST_NAME ${TEST} NAME_WE)
    add_executable(${TEST_NAME} ${TEST})
    target_link_libraries(${TEST_NAME} ${LINKS_FLAGS})
endforeach()
