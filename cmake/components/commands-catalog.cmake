target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/CommandCatalog.cpp
    ${SSG_SOURCE_DIR}/src/CommandReference.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_command_catalog
        ${SSG_SOURCE_DIR}/tests/test_command_catalog.cpp
    )
    target_include_directories(test_command_catalog PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_command_catalog PRIVATE ssg_core)
    add_test(NAME test_command_catalog COMMAND test_command_catalog)

    add_executable(test_commands
        ${SSG_SOURCE_DIR}/tests/test_commands.cpp
    )
    target_include_directories(test_commands PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_compile_definitions(test_commands PRIVATE
        SSG_COMMAND_DOC_PATH="${SSG_SOURCE_DIR}/doc/commands.md"
    )
    target_link_libraries(test_commands PRIVATE ssg_core)
    add_test(NAME test_commands COMMAND test_commands)
endif()
