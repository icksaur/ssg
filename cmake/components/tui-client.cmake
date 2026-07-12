if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_tui_fixture
        ${SSG_SOURCE_DIR}/tests/test_tui_fixture.cpp
        ${SSG_SOURCE_DIR}/examples/tui/tui_fixture.cpp
    )
    target_include_directories(test_tui_fixture PRIVATE
        ${SSG_SOURCE_DIR}/examples/tui
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_tui_fixture PRIVATE
        SSG_TUI_WORKFLOW_PATH="${SSG_SOURCE_DIR}/tests/fixtures/tui/mandatory-workflow.tsv"
        SSG_TUI_SCREEN_PATH="${SSG_SOURCE_DIR}/tests/fixtures/tui/final-screen.txt"
    )
    target_link_libraries(test_tui_fixture PRIVATE ssg)
    add_test(NAME test_tui_fixture COMMAND test_tui_fixture)
endif()
