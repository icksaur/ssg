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
endif()
