if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(editor_benchmark
        ${SSG_SOURCE_DIR}/benchmarks/editor_benchmark.cpp
    )
    target_link_libraries(editor_benchmark PRIVATE ssg_core)
    target_compile_definitions(editor_benchmark PRIVATE
        SSG_PERFORMANCE_MANIFEST="${SSG_SOURCE_DIR}/benchmarks/corpus/manifest.json"
        SSG_PERFORMANCE_CORPUS="${SSG_SOURCE_DIR}/benchmarks/corpus/mixed-code.txt"
        SSG_PERFORMANCE_OPERATIONS="${SSG_SOURCE_DIR}/benchmarks/corpus/operations.tsv"
        SSG_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        SSG_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        SSG_BENCHMARK_BUILD_FLAGS="${CMAKE_CXX_FLAGS_RELEASE}"
    )

    add_test(NAME performance_correctness
             COMMAND editor_benchmark --verify-only)
    set_tests_properties(performance_correctness PROPERTIES
        LABELS "performance-correctness"
        TIMEOUT 120)

    add_test(NAME performance_measurement
             COMMAND editor_benchmark)
    set_tests_properties(performance_measurement PROPERTIES
        LABELS "performance"
        TIMEOUT 300)

    add_executable(protocol_delta_benchmark
        ${SSG_SOURCE_DIR}/benchmarks/protocol_delta_benchmark.cpp
    )
    target_link_libraries(protocol_delta_benchmark PRIVATE ssg_grid ssg_protocol)
    target_compile_definitions(protocol_delta_benchmark PRIVATE
        SSG_PROTOCOL_FIXTURES_DIR="${SSG_SOURCE_DIR}/tests/fixtures/protocol"
    )

    add_test(NAME protocol_delta_correctness
             COMMAND protocol_delta_benchmark --verify-only)
    set_tests_properties(protocol_delta_correctness PROPERTIES
        LABELS "performance-correctness"
        TIMEOUT 120)

    add_test(NAME performance_measurement_protocol_delta
             COMMAND protocol_delta_benchmark --enforce)
    set_tests_properties(performance_measurement_protocol_delta PROPERTIES
        LABELS "performance"
        TIMEOUT 300)
endif()
