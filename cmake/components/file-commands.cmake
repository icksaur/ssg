target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Workspace.cpp
    ${SSG_SOURCE_DIR}/src/FileCommands.cpp
    ${SSG_SOURCE_DIR}/src/FileArchive.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_workspace
        ENTRY ${SSG_SOURCE_DIR}/tests/test_workspace.cpp
        SYMBOL test_workspace)
    ssg_test_link_libraries(test_workspace PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_file_commands
        ENTRY ${SSG_SOURCE_DIR}/tests/test_file_commands.cpp
        SYMBOL test_file_commands)
    ssg_test_link_libraries(test_file_commands PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_path_prompt
        ENTRY ${SSG_SOURCE_DIR}/tests/test_path_prompt.cpp
        SYMBOL test_path_prompt)
    ssg_test_include_directories(test_path_prompt PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_path_prompt PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_name_clash
        ENTRY ${SSG_SOURCE_DIR}/tests/test_name_clash.cpp
        SYMBOL test_name_clash)
    ssg_test_include_directories(test_name_clash PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_name_clash PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_file_archive
        ENTRY ${SSG_SOURCE_DIR}/tests/test_file_archive.cpp
        SYMBOL test_file_archive)
    ssg_test_include_directories(test_file_archive PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_file_archive PRIVATE ssg_core)

endif()
