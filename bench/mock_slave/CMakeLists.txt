# mock_slave_grpc — gRPC scalability test for cluster_master.
# Simulates N slave nodes via ClusterControl::NodeSession bidi streams.

add_executable(mock_slave_grpc mock_slave_grpc.cc)

target_include_directories(mock_slave_grpc PRIVATE
    ${CMAKE_SOURCE_DIR}/common
)

target_link_libraries(mock_slave_grpc PRIVATE
    cluster_proto
    gRPC::grpc++
    Threads::Threads
)
