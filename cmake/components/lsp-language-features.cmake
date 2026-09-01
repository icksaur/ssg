target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/LspFeatureController.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_lsp_features
        ${SSG_SOURCE_DIR}/tests/fake_lsp_server.cpp
        ${SSG_SOURCE_DIR}/tests/test_lsp_features.cpp
    )
    target_include_directories(test_lsp_features PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_lsp_features PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    target_link_libraries(test_lsp_features PRIVATE ssg_core)
    add_test(NAME test_lsp_features COMMAND test_lsp_features)
endif()
