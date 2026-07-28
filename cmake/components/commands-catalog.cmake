target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/Commands.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    # Generates doc/commands.md from the catalog.  A build step because CMake
    # cannot read a C++ table at configure time; the generator holds no command
    # data, only formatting.
    add_executable(ssg_command_docs
        ${SSG_SOURCE_DIR}/tools/command_docs.cpp
    )
    target_link_libraries(ssg_command_docs PRIVATE ssg)

    add_custom_command(
        OUTPUT ${SSG_SOURCE_DIR}/doc/commands.md
        COMMAND ssg_command_docs ${SSG_SOURCE_DIR}/doc/commands.md
        DEPENDS ssg_command_docs ${SSG_SOURCE_DIR}/src/Commands.cpp
        COMMENT "Generating doc/commands.md from the command catalog"
        VERBATIM
    )
    add_custom_target(ssg_command_docs_generate ALL
        DEPENDS ${SSG_SOURCE_DIR}/doc/commands.md
    )

    add_executable(test_commands
        ${SSG_SOURCE_DIR}/tests/test_commands.cpp
    )
    target_include_directories(test_commands PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_compile_definitions(test_commands PRIVATE
        SSG_COMMAND_DOC_PATH="${SSG_SOURCE_DIR}/doc/commands.md"
    )
    target_link_libraries(test_commands PRIVATE ssg)
    add_test(NAME test_commands COMMAND test_commands)
endif()
