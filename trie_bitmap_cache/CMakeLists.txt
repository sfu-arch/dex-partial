cmake_minimum_required(VERSION 3.10.2 FATAL_ERROR)
project(TBC)  # Trie-Bitmap Cache

# ---------------------------------------------------------------
# Build type
# ---------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()

add_definitions(-UNDEBUG)

set(CMAKE_C_FLAGS   "-Wall -Wno-deprecated-declarations -Wno-unused-variable -Wno-unused-but-set-variable -Wsign-compare -mssse3 -march=native -O3")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++17")

if(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
elseif(${CMAKE_BUILD_TYPE} STREQUAL "RelWithDebInfo")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O2")
endif()

# ---------------------------------------------------------------
# Paths — pull in DEX's existing infrastructure directly
# ---------------------------------------------------------------
set(DEX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../dex)

# DEX include directories (same as DEX's own CMakeLists.txt)
include_directories(${DEX_DIR}/include)
include_directories(${DEX_DIR}/util)

# TBC's own headers
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# DEX test directory (for zipf.h, uniform.h, uniform_generator.h)
include_directories(${DEX_DIR}/test)

# ---------------------------------------------------------------
# DEX's static library (DSM, RDMA, connections, etc.)
# Build it from DEX's source files.
# ---------------------------------------------------------------
file(GLOB_RECURSE DEX_SRC ${DEX_DIR}/src/*.cpp)
add_library(DEX_LIB STATIC ${DEX_SRC})

# ---------------------------------------------------------------
# Link flags (same as DEX)
# ---------------------------------------------------------------
set(LINK_FLAGS "-lcityhash -lboost_system -lboost_coroutine -lpthread -libverbs -lmemcached -ltbb -lnuma")

# ---------------------------------------------------------------
# TBC benchmark executable
# ---------------------------------------------------------------
add_executable(tbc_bench test/tbc_bench.cpp)
target_link_libraries(tbc_bench DEX_LIB ${LINK_FLAGS})

# ---------------------------------------------------------------
# TBC unit test (standalone, no RDMA required)
# ---------------------------------------------------------------
add_executable(tbc_unit_test test/tbc_unit_test.cpp)
target_compile_definitions(tbc_unit_test PRIVATE TBC_UNIT_TEST)
# Unit test doesn't need DEX_LIB or RDMA libs

# ---------------------------------------------------------------
# Print config
# ---------------------------------------------------------------
message(STATUS "TBC build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "DEX source dir : ${DEX_DIR}")
message(STATUS "TBC include dir: ${CMAKE_CURRENT_SOURCE_DIR}/include")
