# master_watchdog — runs on master's BF2 ARM, monitors cluster_master health.
# Links: Comch NIC-side C lib, cluster_proto (gRPC stubs), gRPC, pthreads.

# --- Comch NIC C library (DOCA-dependent) ---
option(ENABLE_DOCA "Enable DOCA Comch support" ON)

if(ENABLE_DOCA AND NOT TARGET comch_nic_c)
    add_library(comch_nic_c STATIC
        ${CMAKE_SOURCE_DIR}/tunnel/nic/comch_nic.c
    )
    target_include_directories(comch_nic_c PUBLIC
        ${DOCA_INC_DIR}
        ${CMAKE_SOURCE_DIR}/tunnel
        ${CMAKE_SOURCE_DIR}/common
    )
    target_compile_definitions(comch_nic_c PUBLIC
        COMCH_NIC_DOCA_VER=${COMCH_NIC_DOCA_VER}
    )
    target_link_directories(comch_nic_c PUBLIC ${DOCA_LIB_DIR})
    target_link_libraries(comch_nic_c PUBLIC
        doca_comm_channel
        doca_common
    )
endif()

# --- master_watchdog executable ---
add_executable(master_watchdog master_watchdog.cc)

target_include_directories(master_watchdog PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/tunnel
)

target_link_libraries(master_watchdog PRIVATE
    cluster_proto
    gRPC::grpc++
    Threads::Threads
)

if(ENABLE_DOCA)
    target_link_libraries(master_watchdog PRIVATE comch_nic_c)
    target_include_directories(master_watchdog PRIVATE ${DOCA_INC_DIR})
else()
    target_compile_definitions(master_watchdog PRIVATE NO_DOCA)
endif()
