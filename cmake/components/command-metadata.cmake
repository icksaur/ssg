if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_command_metadata
        ENTRY ${SSG_SOURCE_DIR}/tests/test_command_metadata.cpp
        SYMBOL test_command_metadata)
    ssg_test_include_directories(test_command_metadata PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_command_metadata PRIVATE ssg_core)

endif()
