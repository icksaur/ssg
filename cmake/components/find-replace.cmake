target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/FindReplace.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_find_replace
        ENTRY ${SSG_SOURCE_DIR}/tests/test_find_replace.cpp
        SYMBOL test_find_replace)
    ssg_test_include_directories(test_find_replace PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_find_replace PRIVATE ssg_core)

endif()
