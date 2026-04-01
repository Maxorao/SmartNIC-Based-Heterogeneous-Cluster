# control-plane/slave/CMakeLists.txt
#
# Builds the slave_agent binary (runs on BF2 ARM aarch64).
#
# DOCA is required for Comch host-metric collection.  Set ENABLE_DOCA=OFF
# to compile without it (HostCollector will fail at runtime, but the binary
# can still be tested with BF2Collector + gRPC on a development machine).
#
# Usage:
#   cmake -DENABLE_DOCA=ON  -DDOCA_DIR=/opt/mellanox/doca ...
#   cmake -DENABLE_DOCA=OFF ...   # dev/test build without DOCA

option(ENABLE_DOCA "Link against DOCA libraries for Comch support" ON)

# -----------------------------------------------------------------------
# Comch NIC-side C code (static library)
# -----------------------------------------------------------------------

if(ENABLE_DOCA)
    add_library(comch_nic_c STATIC
        ${CMAKE_SOURCE_DIR}/tunnel/nic/comch_nic.c
    )

    target_include_directories(comch_nic_c PRIVATE
        ${DOCA_INC_DIR}
        ${CMAKE_SOURCE_DIR}/tunnel
        ${CMAKE_SOURCE_DIR}/common
    )

    target_compile_definitions(comch_nic_c PRIVATE
        COMCH_NIC_DOCA_VER=${COMCH_NIC_DOCA_VER}
    )

    # DOCA libraries vary by version.  DOCA 1.5 (BF2) needs:
    #   libdoca_comm_channel, libdoca_common, libdoca_log
    # DOCA 3.x additionally needs:
    #   libdoca_comch, libdoca_core, libdoca_pe, libdoca_log
    if(COMCH_NIC_DOCA_VER LESS 30)
        # DOCA 1.5 link set
        find_library(DOCA_CC_LIB    doca_comm_channel PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)
        find_library(DOCA_COMMON_LIB doca_common      PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)
        find_library(DOCA_LOG_LIB   doca_log          PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)

        if(DOCA_CC_LIB AND DOCA_COMMON_LIB)
            target_link_libraries(comch_nic_c PRIVATE
                ${DOCA_CC_LIB}
                ${DOCA_COMMON_LIB}
            )
            if(DOCA_LOG_LIB)
                target_link_libraries(comch_nic_c PRIVATE ${DOCA_LOG_LIB})
            endif()
        else()
            message(WARNING "DOCA 1.5 libraries not found in ${DOCA_LIB_DIR}")
        endif()
    else()
        # DOCA 3.x link set
        find_library(DOCA_COMCH_LIB doca_comch PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)
        find_library(DOCA_CORE_LIB  doca_core  PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)
        find_library(DOCA_PE_LIB    doca_pe    PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)
        find_library(DOCA_LOG_LIB   doca_log   PATHS ${DOCA_LIB_DIR} NO_DEFAULT_PATH)

        if(DOCA_COMCH_LIB AND DOCA_CORE_LIB)
            target_link_libraries(comch_nic_c PRIVATE
                ${DOCA_COMCH_LIB}
                ${DOCA_CORE_LIB}
            )
            if(DOCA_PE_LIB)
                target_link_libraries(comch_nic_c PRIVATE ${DOCA_PE_LIB})
            endif()
            if(DOCA_LOG_LIB)
                target_link_libraries(comch_nic_c PRIVATE ${DOCA_LOG_LIB})
            endif()
        else()
            message(WARNING "DOCA 3.x libraries not found in ${DOCA_LIB_DIR}")
        endif()
    endif()

    target_include_directories(comch_nic_c PUBLIC
        ${DOCA_INC_DIR}
    )
endif()

# -----------------------------------------------------------------------
# slave_agent executable
# -----------------------------------------------------------------------

add_executable(slave_agent
    slave_agent.cc
    bf2_collector.cc
    host_collector.cc
)

target_include_directories(slave_agent PRIVATE
    ${CMAKE_SOURCE_DIR}/tunnel
    ${CMAKE_SOURCE_DIR}/common
)

target_link_libraries(slave_agent PRIVATE
    cluster_proto
    gRPC::grpc++
    protobuf::libprotobuf
    Threads::Threads
)

if(ENABLE_DOCA)
    target_link_libraries(slave_agent PRIVATE comch_nic_c)
    target_include_directories(slave_agent PRIVATE ${DOCA_INC_DIR})
else()
    # Without DOCA, HostCollector cannot initialise Comch.
    # Provide a stub so the code compiles — runtime init will fail gracefully.
    target_compile_definitions(slave_agent PRIVATE NO_DOCA=1)
    message(STATUS "slave_agent: DOCA disabled — HostCollector will not function")
endif()
