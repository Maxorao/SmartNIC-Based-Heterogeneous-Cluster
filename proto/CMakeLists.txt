# Proto library: generates C++ sources from cluster.proto
# Other targets link against `cluster_proto` to use the generated code.

set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/cluster.proto")

# Output directory for generated files
set(PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# Find protoc and grpc_cpp_plugin
get_target_property(PROTOC_PATH protobuf::protoc LOCATION)
get_target_property(GRPC_PLUGIN_PATH gRPC::grpc_cpp_plugin LOCATION)

# Generated file paths
set(PROTO_SRCS "${PROTO_OUT_DIR}/cluster.pb.cc")
set(PROTO_HDRS "${PROTO_OUT_DIR}/cluster.pb.h")
set(GRPC_SRCS  "${PROTO_OUT_DIR}/cluster.grpc.pb.cc")
set(GRPC_HDRS  "${PROTO_OUT_DIR}/cluster.grpc.pb.h")

# Custom command: run protoc to generate both protobuf and gRPC stubs
add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${PROTOC_PATH}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
        --cpp_out=${PROTO_OUT_DIR}
        --grpc_out=${PROTO_OUT_DIR}
        --plugin=protoc-gen-grpc=${GRPC_PLUGIN_PATH}
        ${PROTO_SRC}
    DEPENDS ${PROTO_SRC}
    COMMENT "Generating C++ protobuf/gRPC stubs from cluster.proto"
)

# Library target
add_library(cluster_proto STATIC
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

target_include_directories(cluster_proto PUBLIC ${PROTO_OUT_DIR})

target_link_libraries(cluster_proto PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)
