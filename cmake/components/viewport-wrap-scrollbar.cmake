target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/Viewport.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_viewport
        ${SSG_SOURCE_DIR}/tests/test_viewport.cpp
    )
    target_link_libraries(test_viewport PRIVATE ssg)
    target_compile_definitions(test_viewport PRIVATE
        VIEWPORT_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/layout/viewports"
    )
    add_test(NAME test_viewport COMMAND test_viewport)

    add_executable(test_scroll_offset
        ${SSG_SOURCE_DIR}/tests/test_scroll_offset.cpp
    )
    target_include_directories(test_scroll_offset PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_scroll_offset PRIVATE ssg)
    add_test(NAME test_scroll_offset COMMAND test_scroll_offset)
endif()
