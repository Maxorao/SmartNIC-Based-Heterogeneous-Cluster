# master_watchdog — runs on master's BF2 ARM, monitors cluster_master health.
# Only built when BUILD_TARGET=BF2. Uses comch_c (comch_nic, DOCA 1.5).

add_executable(master_watchdog master_watchdog.cc)

target_include_directories(master_watchdog PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/tunnel
    ${DOCA_INC_DIR}
)

target_link_libraries(master_watchdog PRIVATE
    cluster_proto
    comch_c
    gRPC::grpc++
    Threads::Threads
)
