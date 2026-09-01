target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/WorkspaceFileIndex.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_workspace_file_index
        ${SSG_SOURCE_DIR}/tests/test_workspace_file_index.cpp
    )
    target_include_directories(test_workspace_file_index PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_workspace_file_index PRIVATE ssg_core)
    add_test(NAME test_workspace_file_index COMMAND test_workspace_file_index)
endif()
