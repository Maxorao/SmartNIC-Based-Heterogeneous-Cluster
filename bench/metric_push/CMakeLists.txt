# metric_push — Enhanced metric shipper with Comch + gRPC fallback.
# Only built when BUILD_TARGET=HOST.

add_executable(metric_push metric_push.cc)

target_include_directories(metric_push PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/tunnel
    ${DOCA_INC_DIR}
)

target_link_libraries(metric_push PRIVATE
    cluster_proto
    comch_c
    gRPC::grpc++
    Threads::Threads
)
