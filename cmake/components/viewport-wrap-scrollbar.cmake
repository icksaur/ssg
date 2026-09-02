target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Viewport.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_viewport
        ENTRY ${SSG_SOURCE_DIR}/tests/test_viewport.cpp
        SYMBOL test_viewport)
    ssg_test_link_libraries(test_viewport PRIVATE ssg_core)
    ssg_test_compile_definitions(test_viewport PRIVATE
        VIEWPORT_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/layout/viewports"
    )

    ssg_add_test_suite(
        NAME test_scroll_offset
        ENTRY ${SSG_SOURCE_DIR}/tests/test_scroll_offset.cpp
        SYMBOL test_scroll_offset)
    ssg_test_include_directories(test_scroll_offset PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_scroll_offset PRIVATE ssg_core)

endif()
