target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/TextInputCommands.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_text_input_commands
        ENTRY ${SSG_SOURCE_DIR}/tests/test_text_input_commands.cpp
        SYMBOL test_text_input_commands)
    ssg_test_link_libraries(test_text_input_commands PRIVATE ssg_core)

endif()
