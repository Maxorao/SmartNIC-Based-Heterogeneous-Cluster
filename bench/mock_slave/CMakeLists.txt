# mock_slave — gRPC scalability test for cluster_master.
# Simulates N slave nodes via ClusterControl::NodeSession bidi streams.

add_executable(mock_slave mock_slave.cc)

target_include_directories(mock_slave PRIVATE
    ${CMAKE_SOURCE_DIR}/common
)

target_link_libraries(mock_slave PRIVATE
    cluster_proto
    gRPC::grpc++
    Threads::Threads
)
