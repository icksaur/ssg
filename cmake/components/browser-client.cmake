if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    find_program(SSG_NODE_EXECUTABLE NAMES node nodejs)
    if(SSG_NODE_EXECUTABLE)
        add_executable(browser_client_fixture
            ${SSG_SOURCE_DIR}/tests/browser/client/fixture.cpp
        )
        target_include_directories(browser_client_fixture PRIVATE
            ${SSG_SOURCE_DIR}/tests
        )
        target_compile_definitions(browser_client_fixture PRIVATE
            SSG_E2E_WORKFLOW_PATH="${SSG_SOURCE_DIR}/tests/fixtures/end_to_end/mandatory-workflow.tsv"
        )
        target_link_libraries(browser_client_fixture PRIVATE ssg_http_server)

        add_test(
            NAME browser_client_source_contract
            COMMAND ${SSG_NODE_EXECUTABLE}
                ${SSG_SOURCE_DIR}/tests/browser/client/source-contract.mjs
                $<TARGET_FILE:browser_client_fixture>
        )
        add_test(
            NAME browser_client_available_runtimes
            COMMAND ${SSG_NODE_EXECUTABLE}
                ${SSG_SOURCE_DIR}/tests/browser/client/run-browser-conformance.mjs
                --server=$<TARGET_FILE:browser_client_fixture>
        )
        add_test(
            NAME browser_client_required_matrix
            COMMAND ${SSG_NODE_EXECUTABLE}
                ${SSG_SOURCE_DIR}/tests/browser/client/run-browser-conformance.mjs
                --server=$<TARGET_FILE:browser_client_fixture>
                --require=chromium,firefox,webkit
        )
        set_tests_properties(
            browser_client_available_runtimes
            PROPERTIES SKIP_RETURN_CODE 77 LABELS browser
        )
        set_tests_properties(
            browser_client_required_matrix
            PROPERTIES LABELS browser
        )
    endif()
endif()
