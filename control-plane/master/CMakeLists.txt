# Build the gRPC-based cluster_master
#
# Targets:
#   cluster_master — main executable
#
# Dependencies:
#   cluster_proto  — generated gRPC/protobuf stubs (from proto/)
#   gRPC::grpc++   — gRPC C++ runtime
#   PostgreSQL     — libpq for TimescaleDB access
#   Threads        — pthreads

# ---------------------------------------------------------------------------
# cluster_master executable
# ---------------------------------------------------------------------------

if(PostgreSQL_FOUND)

add_executable(cluster_master
    cluster_master.cc
    grpc_service.cc
    node_registry.cc
    http_status.cc
    db.cc
)

target_include_directories(cluster_master PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PostgreSQL_INCLUDE_DIRS}
)

target_link_libraries(cluster_master PRIVATE
    cluster_proto
    gRPC::grpc++
    protobuf::libprotobuf
    ${PostgreSQL_LIBRARIES}
    Threads::Threads
)

target_compile_features(cluster_master PRIVATE cxx_std_17)

else()
    message(STATUS "Skipping cluster_master (requires PostgreSQL)")
endif()
