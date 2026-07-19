# M10-1 startup measurement harness (doc/spec-fast-startup.md).
#
# ssg_startup_probe: the app built WITH startup instrumentation compiled in
# (SSG_STARTUP_TRACE_ENABLED). The shipped `ssg` (ssg-app.cmake) has it compiled
# out, so the measured binary carries no instrumentation cost.
#
# startup_benchmark: launches the probe under a pty, attributes exec->first-frame
# time to the cold-start phases, and prints a baseline report.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_startup_probe
        ${SSG_SOURCE_DIR}/apps/ssg_main.cpp
        ${SSG_SOURCE_DIR}/apps/ssg_terminal.cpp
        ${SSG_SOURCE_DIR}/apps/pointer_routing.cpp
    )
    target_include_directories(ssg_startup_probe PRIVATE ${SSG_SOURCE_DIR}/apps)
    target_compile_definitions(ssg_startup_probe PRIVATE SSG_STARTUP_TRACE_ENABLED)
    target_link_libraries(ssg_startup_probe PRIVATE ssg)

    add_executable(startup_benchmark
        ${SSG_SOURCE_DIR}/benchmarks/startup_benchmark.cpp
    )
    target_link_libraries(startup_benchmark PRIVATE util)
    add_dependencies(startup_benchmark ssg_startup_probe ssg_app)
    target_compile_definitions(startup_benchmark PRIVATE
        SSG_STARTUP_PROBE_BINARY="$<TARGET_FILE:ssg_startup_probe>"
        SSG_STARTUP_CLEAN_BINARY="$<TARGET_FILE:ssg_app>"
        SSG_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        SSG_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        SSG_BENCHMARK_BUILD_FLAGS="${CMAKE_CXX_FLAGS_RELEASE}"
    )

    # Timing-free portable oracle: the shipped ssg must emit no startup trace
    # (instrumentation compiled out).  Not the wall-clock measurement itself.
    add_test(NAME startup_trace_compiled_out
             COMMAND startup_benchmark --verify-clean)
    set_tests_properties(startup_trace_compiled_out PROPERTIES
        LABELS "startup"
        TIMEOUT 60)
endif()
