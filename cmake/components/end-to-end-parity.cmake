if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_end_to_end
        ENTRY ${SSG_SOURCE_DIR}/tests/test_end_to_end_session.cpp
        SYMBOL test_end_to_end
        SOURCES
            ${SSG_SOURCE_DIR}/examples/tui/tui_fixture.cpp)
    ssg_test_include_directories(test_end_to_end PRIVATE
        ${SSG_SOURCE_DIR}/examples/tui
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_end_to_end PRIVATE
        SSG_E2E_WORKFLOW_PATH="${SSG_SOURCE_DIR}/tests/fixtures/end_to_end/mandatory-workflow.tsv"
    )
    ssg_test_link_libraries(test_end_to_end PRIVATE ssg_tui_objects)

endif()
