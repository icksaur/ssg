if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_tui_fixture
        ${SSG_SOURCE_DIR}/tests/test_tui_session.cpp
        ${SSG_SOURCE_DIR}/examples/tui/tui_fixture.cpp
    )
    target_include_directories(test_tui_fixture PRIVATE
        ${SSG_SOURCE_DIR}/examples/tui
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_tui_fixture PRIVATE ssg_tui_objects)
    add_test(NAME test_tui_fixture COMMAND test_tui_fixture)
endif()
