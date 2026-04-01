# metric_push — Enhanced metric shipper with Comch + gRPC fallback.
# Links: Comch host C lib, cluster_proto (gRPC stubs), gRPC, pthreads.

option(ENABLE_DOCA "Enable DOCA Comch support" ON)

# --- Comch host C library (DOCA-dependent) ---
if(ENABLE_DOCA AND NOT TARGET comch_host_c)
    add_library(comch_host_c STATIC
        ${CMAKE_SOURCE_DIR}/tunnel/host/comch_host.c
    )
    target_include_directories(comch_host_c PUBLIC
        ${DOCA_INC_DIR}
        ${CMAKE_SOURCE_DIR}/tunnel
    )
    target_compile_definitions(comch_host_c PUBLIC
        COMCH_HOST_DOCA_VER=${COMCH_HOST_DOCA_VER}
    )
    target_link_directories(comch_host_c PUBLIC ${DOCA_LIB_DIR})
    target_link_libraries(comch_host_c PUBLIC
        doca_comm_channel
        doca_common
    )
endif()

# --- metric_push executable ---
add_executable(metric_push metric_push.cc)

target_include_directories(metric_push PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/tunnel
)

target_link_libraries(metric_push PRIVATE
    cluster_proto
    gRPC::grpc++
    Threads::Threads
)

if(ENABLE_DOCA)
    target_link_libraries(metric_push PRIVATE comch_host_c)
    target_include_directories(metric_push PRIVATE ${DOCA_INC_DIR})
else()
    target_compile_definitions(metric_push PRIVATE NO_DOCA)
endif()
