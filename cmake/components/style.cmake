target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Style.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_style
        ENTRY ${SSG_SOURCE_DIR}/tests/test_style.cpp
        SYMBOL test_style)
    ssg_test_include_directories(test_style PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_compile_definitions(test_style PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    ssg_test_link_libraries(test_style PRIVATE ssg_core)

endif()
