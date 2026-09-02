# open-metrics component manifest
#
# Adds the file-open instrumentation (thread-local phase timers + validation /
# tree-materialization counters) to the ssg library, and registers:
#   - test_open_equivalence: the IMMUTABLE open-equivalence golden oracle (the
#     LF-2..LF-4b safety net: byte-identical open across the encoding corpus).
#   - test_open_metrics: the EVOLVING counter oracle (validates-twice today,
#     driven to 1/0 by LF-3a/LF-4b).
#   - open_path_benchmark: the sub-phase attribution + buffered-read calibration
#     measurement, kept off the default ctest run (name matches the
#     `-E performance_measurement` filter).

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/open_metrics.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_open_equivalence
        ENTRY ${SSG_SOURCE_DIR}/tests/test_open_equivalence.cpp
        SYMBOL test_open_equivalence)
    ssg_test_link_libraries(test_open_equivalence PRIVATE ssg_core)
    ssg_test_compile_definitions(test_open_equivalence PRIVATE
        SSG_OPEN_GOLDEN="${SSG_SOURCE_DIR}/tests/fixtures/open/golden.txt")

    ssg_add_test_suite(
        NAME test_open_metrics
        ENTRY ${SSG_SOURCE_DIR}/tests/test_open_metrics.cpp
        SYMBOL test_open_metrics)
    ssg_test_include_directories(test_open_metrics PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_open_metrics PRIVATE ssg_core)

    add_executable(open_path_benchmark
        ${SSG_SOURCE_DIR}/benchmarks/open_path_benchmark.cpp
    )
    target_include_directories(open_path_benchmark PRIVATE
        ${SSG_SOURCE_DIR}/src)
    target_link_libraries(open_path_benchmark PRIVATE ssg_core)
    add_test(NAME performance_measurement_open_path COMMAND open_path_benchmark)
    set_tests_properties(performance_measurement_open_path PROPERTIES
        LABELS "performance"
        TIMEOUT 300)
endif()
