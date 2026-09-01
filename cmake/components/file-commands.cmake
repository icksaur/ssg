target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Workspace.cpp
    ${SSG_SOURCE_DIR}/src/FileCommands.cpp
    ${SSG_SOURCE_DIR}/src/FileArchive.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_workspace
        ${SSG_SOURCE_DIR}/tests/test_workspace.cpp
    )
    target_link_libraries(test_workspace PRIVATE ssg_core)
    add_test(NAME test_workspace COMMAND test_workspace)

    add_executable(test_file_commands
        ${SSG_SOURCE_DIR}/tests/test_file_commands.cpp
    )
    target_link_libraries(test_file_commands PRIVATE ssg_core)
    add_test(NAME test_file_commands COMMAND test_file_commands)

    add_executable(test_path_prompt
        ${SSG_SOURCE_DIR}/tests/test_path_prompt.cpp
    )
    target_include_directories(test_path_prompt PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_path_prompt PRIVATE ssg_core)
    add_test(NAME test_path_prompt COMMAND test_path_prompt)

    add_executable(test_name_clash
        ${SSG_SOURCE_DIR}/tests/test_name_clash.cpp
    )
    target_include_directories(test_name_clash PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_name_clash PRIVATE ssg_core)
    add_test(NAME test_name_clash COMMAND test_name_clash)

    add_executable(test_file_archive
        ${SSG_SOURCE_DIR}/tests/test_file_archive.cpp
    )
    target_include_directories(test_file_archive PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_file_archive PRIVATE ssg_core)
    add_test(NAME test_file_archive COMMAND test_file_archive)
endif()
