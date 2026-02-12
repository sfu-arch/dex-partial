cmake_minimum_required(VERSION 3.10.2 FATAL_ERROR)
project(APEX)

# ============================================================================
# Compiler Options
# ============================================================================
set(CMAKE_C_FLAGS "-Wall -Wno-deprecated-declarations -Wno-unused-variable -Wno-unused-but-set-variable -Wsign-compare -O3")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++17 -mssse3 -march=native")

# ============================================================================
# RDMA Driver Detection
# ============================================================================
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

message(STATUS "")
message(STATUS "=== APEX RDMA Configuration ===")
if(FORCE_EXP_VERBS OR (HAS_VERBS_EXP AND NOT FORCE_STD_VERBS))
    message(STATUS "  Transport:      DC (Dynamically Connected)")
    message(STATUS "  Device Memory:  Enabled")
    message(STATUS "  Masked Atomics: Enabled")
else()
    message(STATUS "  Transport:      RC (Reliably Connected) - FALLBACK")
    message(STATUS "  Device Memory:  Emulated with host memory")
    message(STATUS "  Masked Atomics: Standard atomics only")
endif()
message(STATUS "================================")

# ============================================================================
# Link Options
# ============================================================================
set(LINKS_FLAGS "-lnuma -lcityhash -lboost_coroutine -lboost_context -lpthread -libverbs -lmemcached -ltbb")

# ============================================================================
# APEX-Specific Build Options
# ============================================================================
option(ENABLE_CORO     "Turn on coroutine support"         ON)
option(ENABLE_CACHE    "Turn on computing-side cache"       ON)
option(STATIC_MN_IP    "Use static MN IPs"                 OFF)

# APEX component toggles
option(ENABLE_VEASM    "Enable Value-Embedded ASM (5MB L3 cache tier)" ON)
option(ENABLE_VCS      "Enable Version Chain Sync"         ON)
option(ENABLE_BLOOM    "Enable bloom filter in leaf pages"  ON)

if(STATIC_MN_IP)
    add_definitions(-DSTATIC_ID_FROM_IP)
endif()

if(ENABLE_CORO)
    add_definitions(-DUSE_CORO)
endif()

if(ENABLE_CACHE)
    add_definitions(-DTREE_ENABLE_CACHE)
endif()

if(ENABLE_VEASM)
    add_definitions(-DAPEX_ENABLE_VEASM)
endif()

if(ENABLE_VCS)
    add_definitions(-DAPEX_ENABLE_VCS)
endif()

if(ENABLE_BLOOM)
    add_definitions(-DAPEX_ENABLE_BLOOM)
endif()

# ============================================================================
# Include Files
# ============================================================================
set(INCLUDE_BASE ${PROJECT_SOURCE_DIR}/include)
include_directories(${INCLUDE_BASE})

# ============================================================================
# Source Files
# ============================================================================
set(COMMON_SRC ${PROJECT_SOURCE_DIR}/src)
file(GLOB_RECURSE COMMON_FILE ${COMMON_SRC}/*.cpp)
add_library(APEX STATIC ${COMMON_FILE})
link_libraries(APEX)

# ============================================================================
# Test Executables
# ============================================================================
file(GLOB TEST_SRC ${PROJECT_SOURCE_DIR}/test/*.cpp)
foreach(TEST ${TEST_SRC})
    get_filename_component(TEST_NAME ${TEST} NAME_WE)
    add_executable(${TEST_NAME} ${TEST})
    target_link_libraries(${TEST_NAME} ${LINKS_FLAGS})
endforeach()
