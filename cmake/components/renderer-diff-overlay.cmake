if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_renderer_diff_overlay
        ${SSG_SOURCE_DIR}/tests/test_renderer_diff_overlay.cpp)
    target_include_directories(test_renderer_diff_overlay
        PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_renderer_diff_overlay PRIVATE ssg_grid)
    add_test(NAME test_renderer_diff_overlay COMMAND test_renderer_diff_overlay)
endif()
