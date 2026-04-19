cmake_minimum_required(VERSION 3.16)
project(SmartNIC_ClusterControl LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Build target selection:
#   -DBUILD_TARGET=HOST  (default)  x86 host: cluster_master, metric_push, mock_slave
#   -DBUILD_TARGET=BF2              BF2 ARM:  slave_agent, master_watchdog
# ---------------------------------------------------------------------------

set(BUILD_TARGET "HOST" CACHE STRING "Build target: HOST or BF2")
message(STATUS "BUILD_TARGET = ${BUILD_TARGET}")

# ---------------------------------------------------------------------------
# External dependencies (common)
# ---------------------------------------------------------------------------

find_package(Threads REQUIRED)

# Protobuf: try CONFIG, fall back to MODULE
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()

# gRPC: try CONFIG, fall back to pkg-config
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPC REQUIRED grpc++ grpc)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
    set(GRPC_FOUND_VIA_PKGCONFIG TRUE)

    if(NOT TARGET gRPC::grpc++)
        add_library(gRPC::grpc++ INTERFACE IMPORTED)
        set_target_properties(gRPC::grpc++ PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${GRPC_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${GRPC_LIBRARIES}"
            INTERFACE_LINK_DIRECTORIES "${GRPC_LIBRARY_DIRS}"
        )
        # Also add to global link directories for transitive deps (abseil etc.)
        link_directories(${GRPC_LIBRARY_DIRS})
    endif()
    if(NOT TARGET gRPC::grpc_cpp_plugin)
        add_executable(gRPC::grpc_cpp_plugin IMPORTED)
        set_target_properties(gRPC::grpc_cpp_plugin PROPERTIES
            IMPORTED_LOCATION "${GRPC_CPP_PLUGIN}"
        )
    endif()
endif()

# PostgreSQL (optional — only needed by cluster_master on HOST)
if(BUILD_TARGET STREQUAL "HOST")
    find_package(PostgreSQL QUIET)
    if(PostgreSQL_FOUND)
        message(STATUS "PostgreSQL found: ${PostgreSQL_INCLUDE_DIRS}")
    else()
        message(STATUS "PostgreSQL not found — cluster_master DB support disabled")
    endif()
endif()

# ---------------------------------------------------------------------------
# DOCA SDK
# ---------------------------------------------------------------------------

set(DOCA_DIR "/opt/mellanox/doca" CACHE PATH "DOCA SDK root directory")
set(DOCA_INC_DIR "${DOCA_DIR}/include" CACHE PATH "DOCA include path")

if(BUILD_TARGET STREQUAL "HOST")
    # Host: DOCA 3.1 — libdoca_comch (x86)
    set(DOCA_ARCH "x86_64-linux-gnu")
    set(DOCA_LIB_DIR "${DOCA_DIR}/lib/${DOCA_ARCH}" CACHE PATH "DOCA library path")
    set(COMCH_DOCA_VER 31)
    message(STATUS "DOCA: Host mode — DOCA 3.1 (comch_host)")
else()
    # BF2: DOCA 1.5 — libdoca_comm_channel (aarch64)
    set(DOCA_ARCH "aarch64-linux-gnu")
    set(DOCA_LIB_DIR "${DOCA_DIR}/lib/${DOCA_ARCH}" CACHE PATH "DOCA library path")
    set(COMCH_DOCA_VER 15)
    message(STATUS "DOCA: BF2 mode — DOCA 1.5 (comch_nic)")
endif()

# ---------------------------------------------------------------------------
# Comch static library (architecture-dependent)
# ---------------------------------------------------------------------------

if(BUILD_TARGET STREQUAL "HOST")
    # comch_host_c: DOCA 3.1 host-side Comch
    add_library(comch_c STATIC
        ${CMAKE_SOURCE_DIR}/tunnel/host/comch_host.c
    )
    target_include_directories(comch_c PUBLIC
        ${DOCA_INC_DIR}
        ${CMAKE_SOURCE_DIR}/tunnel
        ${CMAKE_SOURCE_DIR}/common
    )
    target_compile_definitions(comch_c PUBLIC COMCH_HOST_DOCA_VER=${COMCH_DOCA_VER})
    target_link_directories(comch_c PUBLIC ${DOCA_LIB_DIR})

    find_library(DOCA_COMCH_LIB doca_comch PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)
    find_library(DOCA_COMMON_LIB doca_common PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)
    if(DOCA_COMCH_LIB AND DOCA_COMMON_LIB)
        target_link_libraries(comch_c PUBLIC ${DOCA_COMCH_LIB} ${DOCA_COMMON_LIB})
        message(STATUS "DOCA 3.1 libs: ${DOCA_COMCH_LIB} ${DOCA_COMMON_LIB}")
    else()
        message(WARNING "DOCA 3.1 libraries not found in ${DOCA_LIB_DIR}")
    endif()
else()
    # comch_nic_c: DOCA 1.5 NIC-side Comch
    add_library(comch_c STATIC
        ${CMAKE_SOURCE_DIR}/tunnel/nic/comch_nic.c
    )
    target_include_directories(comch_c PUBLIC
        ${DOCA_INC_DIR}
        ${CMAKE_SOURCE_DIR}/tunnel
        ${CMAKE_SOURCE_DIR}/common
    )
    target_compile_definitions(comch_c PUBLIC COMCH_NIC_DOCA_VER=${COMCH_DOCA_VER})
    target_link_directories(comch_c PUBLIC ${DOCA_LIB_DIR})

    find_library(DOCA_CC_LIB doca_comm_channel PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)
    find_library(DOCA_COMMON_LIB doca_common PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)
    if(DOCA_CC_LIB AND DOCA_COMMON_LIB)
        target_link_libraries(comch_c PUBLIC ${DOCA_CC_LIB} ${DOCA_COMMON_LIB})
        message(STATUS "DOCA 1.5 libs: ${DOCA_CC_LIB} ${DOCA_COMMON_LIB}")
    else()
        message(WARNING "DOCA 1.5 libraries not found in ${DOCA_LIB_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Common include paths
# ---------------------------------------------------------------------------

include_directories(${CMAKE_SOURCE_DIR}/common)

# ---------------------------------------------------------------------------
# Subdirectories — conditional on BUILD_TARGET
# ---------------------------------------------------------------------------

add_subdirectory(proto)

if(BUILD_TARGET STREQUAL "HOST")
    add_subdirectory(control-plane/master)
    add_subdirectory(bench/metric_push)
    add_subdirectory(bench/mock_slave)
    # e2e_host lives under bench/rdma_e2e and builds on HOST
    add_subdirectory(bench/rdma_e2e)
else()
    add_subdirectory(control-plane/slave)
    add_subdirectory(control-plane/watchdog)
    # RDMA bridges + orchestrator agent only on BF2
    add_subdirectory(control-plane/rdma_bridge)
    add_subdirectory(control-plane/orchestrator_agent)
    # e2e_nic lives under bench/rdma_e2e and builds on BF2
    add_subdirectory(bench/rdma_e2e)
endif()

# Note: tunnel/, bench/latency_bench/, bench/gemm_bench/ keep their
# Makefiles and are not part of the CMake build.
