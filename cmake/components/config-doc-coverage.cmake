if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_config_doc
        ${SSG_SOURCE_DIR}/tests/test_config_doc.cpp
    )
    target_include_directories(test_config_doc PRIVATE
        ${SSG_SOURCE_DIR}/include
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_config_doc PRIVATE
        SSG_CONFIG_DOC_PATH="${SSG_SOURCE_DIR}/doc/config.md"
    )
    target_link_libraries(test_config_doc PRIVATE ssg)
    add_test(NAME test_config_doc COMMAND test_config_doc)
endif()
