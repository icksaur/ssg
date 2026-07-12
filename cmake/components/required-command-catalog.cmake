if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_required_commands
        ${SSG_SOURCE_DIR}/tests/test_required_commands.cpp
    )
    target_include_directories(test_required_commands PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_required_commands PRIVATE
        SSG_REQUIRED_COMMANDS_PATH="${SSG_SOURCE_DIR}/data/required-commands.json"
    )
    add_test(NAME test_required_commands COMMAND test_required_commands)
endif()
