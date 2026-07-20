target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/TextInputCommands.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_text_input_commands
        ${SSG_SOURCE_DIR}/tests/test_text_input_commands.cpp
    )
    target_link_libraries(test_text_input_commands PRIVATE ssg)
    add_test(NAME test_text_input_commands COMMAND test_text_input_commands)
endif()
