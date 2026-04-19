# orchestrator_agent — runs on each BF2 to execute local migration
# commands via gRPC (replacement for two-hop SSH).

add_executable(orchestrator_agent orchestrator_agent.cc)

target_include_directories(orchestrator_agent PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_BINARY_DIR}/proto
)

target_link_libraries(orchestrator_agent PRIVATE
    oa_proto
    gRPC::grpc++
    Threads::Threads
)

target_compile_options(orchestrator_agent PRIVATE -O2 -Wall)
install(TARGETS orchestrator_agent RUNTIME DESTINATION bin)
