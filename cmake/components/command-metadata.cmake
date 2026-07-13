target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/command_metadata.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_command_metadata
        ${SSG_SOURCE_DIR}/tests/test_command_metadata.cpp
    )
    target_include_directories(test_command_metadata PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_command_metadata PRIVATE ssg)
    add_test(NAME test_command_metadata COMMAND test_command_metadata)
endif()
