target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/lsp_sync_client.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_lsp_sync
        ENTRY ${SSG_SOURCE_DIR}/tests/test_lsp_sync.cpp
        SYMBOL test_lsp_sync
        SOURCES
            ${SSG_SOURCE_DIR}/tests/fake_lsp_server.cpp)
    ssg_test_include_directories(test_lsp_sync PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_lsp_sync PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    ssg_test_link_libraries(test_lsp_sync PRIVATE ssg_core)

endif()
