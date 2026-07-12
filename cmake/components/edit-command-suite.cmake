target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/edit_commands.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_edit_commands
        ${SSG_SOURCE_DIR}/tests/test_edit_commands.cpp
    )
    target_link_libraries(test_edit_commands PRIVATE ssg)
    add_test(NAME test_edit_commands COMMAND test_edit_commands)
endif()
