if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_end_to_end
        ${SSG_SOURCE_DIR}/tests/test_end_to_end_session.cpp
        ${SSG_SOURCE_DIR}/examples/tui/tui_fixture.cpp
    )
    target_include_directories(test_end_to_end PRIVATE
        ${SSG_SOURCE_DIR}/examples/tui
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_end_to_end PRIVATE
        SSG_E2E_WORKFLOW_PATH="${SSG_SOURCE_DIR}/tests/fixtures/end_to_end/mandatory-workflow.tsv"
    )
    target_link_libraries(test_end_to_end PRIVATE ssg ssg_http_server)
    add_test(NAME test_end_to_end COMMAND test_end_to_end)
endif()
