target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/EditorSessionBuilder.cpp
    ${SSG_SOURCE_DIR}/src/StatusFields.cpp
    ${SSG_SOURCE_DIR}/src/session_snapshot.cpp
)
target_compile_definitions(ssg PRIVATE
    SSG_STATUS_FIELDS_PATH="${SSG_SOURCE_DIR}/data/ui/status_fields.json"
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_editor_session_assembly
        ${SSG_SOURCE_DIR}/tests/test_editor_session_assembly.cpp
    )
    target_include_directories(test_editor_session_assembly PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_editor_session_assembly PRIVATE
        SSG_REQUIRED_COMMANDS_PATH="${SSG_SOURCE_DIR}/data/required-commands.json"
    )
    target_link_libraries(test_editor_session_assembly PRIVATE ssg)
    add_test(NAME test_editor_session_assembly
             COMMAND test_editor_session_assembly)
endif()
