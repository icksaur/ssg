target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/workspace.cpp
    ${SSG_SOURCE_DIR}/src/file_commands.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_workspace
        ${SSG_SOURCE_DIR}/tests/test_workspace.cpp
    )
    target_link_libraries(test_workspace PRIVATE ssg)
    add_test(NAME test_workspace COMMAND test_workspace)

    add_executable(test_file_commands
        ${SSG_SOURCE_DIR}/tests/test_file_commands.cpp
    )
    target_link_libraries(test_file_commands PRIVATE ssg)
    add_test(NAME test_file_commands COMMAND test_file_commands)
endif()
