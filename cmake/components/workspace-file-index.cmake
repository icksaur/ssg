target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/WorkspaceFileIndex.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_workspace_file_index
        ENTRY ${SSG_SOURCE_DIR}/tests/test_workspace_file_index.cpp
        SYMBOL test_workspace_file_index)
    ssg_test_include_directories(test_workspace_file_index PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_workspace_file_index PRIVATE ssg_core)

endif()
