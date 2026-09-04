if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_grid_presentation_builder
        ENTRY ${SSG_SOURCE_DIR}/tests/test_grid_presentation_builder.cpp
        SYMBOL test_grid_presentation_builder)
    ssg_test_include_directories(test_grid_presentation_builder PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_grid_presentation_builder PRIVATE ssg_tui_objects)

endif()
