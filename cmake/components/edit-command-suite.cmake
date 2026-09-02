target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/EditCommands.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_edit_commands
        ENTRY ${SSG_SOURCE_DIR}/tests/test_edit_commands.cpp
        SYMBOL test_edit_commands)
    ssg_test_link_libraries(test_edit_commands PRIVATE ssg_core)

endif()
