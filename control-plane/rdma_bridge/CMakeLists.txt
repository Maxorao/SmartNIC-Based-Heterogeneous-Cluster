# control-plane/rdma_bridge — RDMA hot-path for control-plane metrics
#
# Two binaries, both for BF2 (aarch64) target:
#   rdma_bridge_slave  — on worker BF2, forwards UDS -> RDMA
#   rdma_bridge_master — on master BF2, forwards RDMA -> gRPC DirectPush

if(NOT BUILD_TARGET STREQUAL "BF2")
    # Bridges only run on BF2 — skip on host build
    return()
endif()

find_library(IBVERBS_LIB ibverbs)
find_library(RDMACM_LIB  rdmacm)
if(NOT IBVERBS_LIB OR NOT RDMACM_LIB)
    message(WARNING "libibverbs/librdmacm not found — rdma_bridge skipped")
    return()
endif()

# Shared RDMA transport object — if not already created by bench/rdma_e2e
if(NOT TARGET rdma_transport)
    add_library(rdma_transport STATIC
        ${CMAKE_SOURCE_DIR}/common/rdma_transport.c
    )
    target_include_directories(rdma_transport PUBLIC
        ${CMAKE_SOURCE_DIR}/common
    )
    target_link_libraries(rdma_transport PUBLIC ${IBVERBS_LIB} ${RDMACM_LIB})
    target_compile_options(rdma_transport PRIVATE -O2 -Wall)
endif()

# ---- rdma_bridge_slave ----
add_executable(rdma_bridge_slave rdma_bridge_slave.cc)
target_include_directories(rdma_bridge_slave PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/control-plane/rdma_bridge
)
target_link_libraries(rdma_bridge_slave PRIVATE
    rdma_transport Threads::Threads
)
target_compile_options(rdma_bridge_slave PRIVATE -O2 -Wall)
install(TARGETS rdma_bridge_slave RUNTIME DESTINATION bin)

# ---- rdma_bridge_master ----
add_executable(rdma_bridge_master rdma_bridge_master.cc)
target_include_directories(rdma_bridge_master PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/control-plane/rdma_bridge
    ${CMAKE_BINARY_DIR}/proto
)
target_link_libraries(rdma_bridge_master PRIVATE
    cluster_proto rdma_transport gRPC::grpc++ Threads::Threads
)
target_compile_options(rdma_bridge_master PRIVATE -O2 -Wall)
install(TARGETS rdma_bridge_master RUNTIME DESTINATION bin)
