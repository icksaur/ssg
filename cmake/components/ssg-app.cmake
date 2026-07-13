# The `ssg` terminal editor application.  Owns terminal I/O only; all editor
# and layout behavior is in the ssg library.  render_screen() is reused from the
# TUI reference adapter until it is promoted into the library.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_app
        ${SSG_SOURCE_DIR}/apps/ssg_main.cpp
        ${SSG_SOURCE_DIR}/apps/ssg_terminal.cpp
        ${SSG_SOURCE_DIR}/examples/tui/tui_fixture.cpp
    )
    set_target_properties(ssg_app PROPERTIES OUTPUT_NAME ssg)
    target_include_directories(ssg_app PRIVATE
        ${SSG_SOURCE_DIR}/apps
        ${SSG_SOURCE_DIR}/examples/tui
    )
    target_link_libraries(ssg_app PRIVATE ssg)

    add_executable(test_ssg_app
        ${SSG_SOURCE_DIR}/tests/test_ssg_app.cpp
        ${SSG_SOURCE_DIR}/apps/ssg_terminal.cpp
        ${SSG_SOURCE_DIR}/examples/tui/tui_fixture.cpp
    )
    target_include_directories(test_ssg_app PRIVATE
        ${SSG_SOURCE_DIR}/apps
        ${SSG_SOURCE_DIR}/examples/tui
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_ssg_app PRIVATE ssg)
    add_test(NAME test_ssg_app COMMAND test_ssg_app)
endif()
