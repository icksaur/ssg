if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    find_program(SSG_NODE_EXECUTABLE NAMES node nodejs)
    if(SSG_NODE_EXECUTABLE)
        add_test(
            NAME browser_input_source_contract
            COMMAND ${SSG_NODE_EXECUTABLE}
                ${SSG_SOURCE_DIR}/tests/browser/input/source-contract.mjs
        )
        add_test(
            NAME browser_input_available_runtimes
            COMMAND ${SSG_NODE_EXECUTABLE}
                ${SSG_SOURCE_DIR}/tests/browser/input/run-browser-conformance.mjs
        )
        add_test(
            NAME browser_input_required_matrix
            COMMAND ${SSG_NODE_EXECUTABLE}
                ${SSG_SOURCE_DIR}/tests/browser/input/run-browser-conformance.mjs
                --require=chromium,firefox,webkit
        )
        set_tests_properties(
            browser_input_available_runtimes
            PROPERTIES SKIP_RETURN_CODE 77 LABELS browser
        )
        set_tests_properties(
            browser_input_required_matrix
            PROPERTIES LABELS browser
        )
    endif()
endif()
