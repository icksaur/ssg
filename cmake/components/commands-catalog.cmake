target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/CommandCatalog.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_command_catalog
        ENTRY ${SSG_SOURCE_DIR}/tests/test_command_catalog.cpp
        SYMBOL test_command_catalog)
    ssg_test_include_directories(test_command_catalog PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_command_catalog PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_commands
        ENTRY ${SSG_SOURCE_DIR}/tests/test_commands.cpp
        SYMBOL test_commands)
    ssg_test_include_directories(test_commands PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/src
    )
    ssg_test_compile_definitions(test_commands PRIVATE
        SSG_COMMAND_DOC_PATH="${SSG_SOURCE_DIR}/doc/commands.md"
    )
    ssg_test_link_libraries(test_commands PRIVATE ssg_core)

endif()
