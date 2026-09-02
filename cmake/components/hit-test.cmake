if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_hit_test
        ENTRY ${SSG_SOURCE_DIR}/tests/test_hit_test.cpp
        SYMBOL test_hit_test)
    ssg_test_include_directories(test_hit_test PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_hit_test PRIVATE ssg_tui_objects)

endif()
