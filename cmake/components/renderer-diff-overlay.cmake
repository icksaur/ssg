if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_renderer_diff_overlay
        ENTRY ${SSG_SOURCE_DIR}/tests/test_renderer_diff_overlay.cpp
        SYMBOL test_renderer_diff_overlay)
    ssg_test_include_directories(test_renderer_diff_overlay
        PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_renderer_diff_overlay PRIVATE ssg_tui_objects)

endif()
