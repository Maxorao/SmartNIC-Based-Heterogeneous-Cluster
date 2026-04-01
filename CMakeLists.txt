cmake_minimum_required(VERSION 3.16)
project(SmartNIC_ClusterControl LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# External dependencies
# ---------------------------------------------------------------------------

find_package(Threads REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# PostgreSQL (optional — only needed by cluster_master)
find_package(PostgreSQL QUIET)
if(PostgreSQL_FOUND)
    message(STATUS "PostgreSQL found: ${PostgreSQL_INCLUDE_DIRS}")
else()
    message(STATUS "PostgreSQL not found — cluster_master DB support disabled")
endif()

# ---------------------------------------------------------------------------
# DOCA SDK paths (used by tunnel and DOCA-dependent components)
# ---------------------------------------------------------------------------

set(DOCA_DIR "/opt/mellanox/doca" CACHE PATH "DOCA SDK root directory")
set(COMCH_HOST_DOCA_VER 31 CACHE STRING "Host-side DOCA version (31=3.x)")
set(COMCH_NIC_DOCA_VER  15 CACHE STRING "NIC-side DOCA version (15=1.5)")

# Detect architecture for conditional DOCA library selection
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(DOCA_ARCH "aarch64-linux-gnu")
    set(IS_ARM TRUE)
else()
    set(DOCA_ARCH "x86_64-linux-gnu")
    set(IS_ARM FALSE)
endif()

set(DOCA_LIB_DIR "${DOCA_DIR}/lib/${DOCA_ARCH}" CACHE PATH "DOCA library path")
set(DOCA_INC_DIR "${DOCA_DIR}/include" CACHE PATH "DOCA include path")

# ---------------------------------------------------------------------------
# Common include paths
# ---------------------------------------------------------------------------

include_directories(${CMAKE_SOURCE_DIR}/common)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------

add_subdirectory(proto)
add_subdirectory(control-plane/master)
add_subdirectory(control-plane/slave)
add_subdirectory(control-plane/watchdog)
add_subdirectory(bench/metric_push)
add_subdirectory(bench/mock_slave)

# Note: tunnel/, bench/latency_bench/, bench/gemm_bench/ keep their
# Makefiles and are not part of the CMake build. Build them separately
# with `make` in their respective directories.
