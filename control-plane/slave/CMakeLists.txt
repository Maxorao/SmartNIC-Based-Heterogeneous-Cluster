# control-plane/slave/CMakeLists.txt
# Builds slave_agent (runs on BF2 ARM). Only built when BUILD_TARGET=BF2.
# Uses comch_c (comch_nic, DOCA 1.5) from top-level CMakeLists.

add_executable(slave_agent
    slave_agent.cc
    bf2_collector.cc
    host_collector.cc
    rdma_uds_client.cc
)

target_include_directories(slave_agent PRIVATE
    ${CMAKE_SOURCE_DIR}/tunnel
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/control-plane/rdma_bridge
    ${DOCA_INC_DIR}
)

target_link_libraries(slave_agent PRIVATE
    cluster_proto
    comch_c
    gRPC::grpc++
    Threads::Threads
)
