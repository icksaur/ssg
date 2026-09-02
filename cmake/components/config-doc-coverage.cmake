if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_config_doc
        ENTRY ${SSG_SOURCE_DIR}/tests/test_config_doc.cpp
        SYMBOL test_config_doc)
    ssg_test_include_directories(test_config_doc PRIVATE
        ${SSG_SOURCE_DIR}/include
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_config_doc PRIVATE
        SSG_CONFIG_DOC_PATH="${SSG_SOURCE_DIR}/doc/config.md"
    )
    ssg_test_link_libraries(test_config_doc PRIVATE ssg_core)

endif()
