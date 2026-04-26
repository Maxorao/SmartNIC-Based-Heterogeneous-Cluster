# bench/rdma_e2e — Chained Comch ↔ RDMA ↔ Comch latency benchmark
#
# Produces:
#   HOST target → e2e_host   (x86 / DOCA 3.1)
#   BF2 target  → e2e_nic    (aarch64 / DOCA 1.5 + RoCE)

# Find ibverbs and rdmacm (available via MOFED or distro rdma-core)
find_library(IBVERBS_LIB  ibverbs)
find_library(RDMACM_LIB   rdmacm)

if(NOT IBVERBS_LIB OR NOT RDMACM_LIB)
    message(WARNING "libibverbs / librdmacm not found — rdma_e2e will be skipped")
    return()
endif()

# rdma_transport object (shared C source between e2e_host/e2e_nic/bridges)
# Guard: on BF2 builds, rdma_bridge may already define this target
if(NOT TARGET rdma_transport)
    add_library(rdma_transport STATIC
        ${CMAKE_SOURCE_DIR}/common/rdma_transport.c
    )
    target_include_directories(rdma_transport PUBLIC
        ${CMAKE_SOURCE_DIR}/common
    )
    target_link_libraries(rdma_transport PUBLIC ${IBVERBS_LIB} ${RDMACM_LIB})
    target_compile_options(rdma_transport PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
endif()

if(BUILD_TARGET STREQUAL "HOST")
    # Host-side binary: uses Comch host library
    add_executable(e2e_host e2e_host.c)
    target_link_libraries(e2e_host PRIVATE comch_c Threads::Threads)
    target_compile_options(e2e_host PRIVATE -O2 -Wall)
    install(TARGETS e2e_host RUNTIME DESTINATION bin)
else()
    # BF2-side binary: uses Comch NIC library + RDMA
    add_executable(e2e_nic e2e_nic.c)
    target_link_libraries(e2e_nic PRIVATE comch_c rdma_transport Threads::Threads)
    target_compile_options(e2e_nic PRIVATE -O2 -Wall)
    install(TARGETS e2e_nic RUNTIME DESTINATION bin)
endif()
