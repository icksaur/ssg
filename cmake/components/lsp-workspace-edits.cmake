target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/lsp_workspace_edit_controller.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_lsp_workspace_edit
        ${SSG_SOURCE_DIR}/tests/fake_lsp_server.cpp
        ${SSG_SOURCE_DIR}/tests/test_lsp_workspace_edit.cpp
    )
    target_include_directories(test_lsp_workspace_edit PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_lsp_workspace_edit PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    target_link_libraries(test_lsp_workspace_edit PRIVATE ssg)
    add_test(NAME test_lsp_workspace_edit COMMAND test_lsp_workspace_edit)
endif()
