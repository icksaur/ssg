if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_end_to_end
        ${SSG_SOURCE_DIR}/tests/test_end_to_end.cpp
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

    find_program(SSG_NODE_EXECUTABLE NAMES node nodejs)
    if(SSG_NODE_EXECUTABLE)
        add_test(
            NAME browser_e2e_parity_available_runtimes
            COMMAND ${SSG_NODE_EXECUTABLE}
                ${SSG_SOURCE_DIR}/tests/browser/end_to_end/run-parity-conformance.mjs
                --server=$<TARGET_FILE:browser_client_fixture>
        )
        add_test(
            NAME browser_e2e_parity_required_matrix
            COMMAND ${SSG_NODE_EXECUTABLE}
                ${SSG_SOURCE_DIR}/tests/browser/end_to_end/run-parity-conformance.mjs
                --server=$<TARGET_FILE:browser_client_fixture>
                --require=chromium,firefox,webkit
        )
        set_tests_properties(
            browser_e2e_parity_available_runtimes
            PROPERTIES SKIP_RETURN_CODE 77 LABELS browser
        )
        set_tests_properties(
            browser_e2e_parity_required_matrix
            PROPERTIES LABELS browser
        )
    endif()
endif()
