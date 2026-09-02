target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/DiffModel.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_diff
        ENTRY ${SSG_SOURCE_DIR}/tests/test_diff.cpp
        SYMBOL test_diff)
    ssg_test_include_directories(test_diff PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_diff PRIVATE
        SSG_DIFF_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/diff"
    )
    ssg_test_link_libraries(test_diff PRIVATE ssg_core)

endif()
