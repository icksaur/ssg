if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_tui_fixture
        ENTRY ${SSG_SOURCE_DIR}/tests/test_tui_session.cpp
        SYMBOL test_tui_fixture
        SOURCES
            ${SSG_SOURCE_DIR}/examples/tui/tui_fixture.cpp)
    ssg_test_include_directories(test_tui_fixture PRIVATE
        ${SSG_SOURCE_DIR}/examples/tui
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_tui_fixture PRIVATE ssg_tui_objects)

endif()
