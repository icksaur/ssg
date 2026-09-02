target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/EditorSession.cpp
    ${SSG_SOURCE_DIR}/src/ViewportProjection.cpp
    ${SSG_SOURCE_DIR}/src/runtime/editing.cpp
    ${SSG_SOURCE_DIR}/src/runtime/files.cpp
    ${SSG_SOURCE_DIR}/src/runtime/help.cpp
    ${SSG_SOURCE_DIR}/src/runtime/language_services.cpp
    ${SSG_SOURCE_DIR}/src/runtime/navigation.cpp
    ${SSG_SOURCE_DIR}/src/runtime/presentation.cpp
    ${SSG_SOURCE_DIR}/src/runtime/snapshot.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_session_snapshot
        ${SSG_SOURCE_DIR}/tests/session/test_session_snapshot.cpp
    )
    target_compile_definitions(test_session_snapshot PRIVATE
        SSG_SOURCE_SCAN_ROOT="${SSG_SOURCE_DIR}"
    )
    target_include_directories(test_session_snapshot PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_session_snapshot PRIVATE ssg_tui_objects)
    add_test(NAME test_session_snapshot COMMAND test_session_snapshot)

    add_executable(test_syntax_injection
        ${SSG_SOURCE_DIR}/tests/test_syntax_injection.cpp
    )
    target_include_directories(test_syntax_injection PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_syntax_injection PRIVATE ssg_core)
    add_test(NAME test_syntax_injection COMMAND test_syntax_injection)

    add_executable(test_session_files
        ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
    )
    target_link_libraries(test_session_files PRIVATE ssg_tui_objects)
    add_test(NAME test_session_files COMMAND test_session_files)

    add_executable(test_session_external_modification
        ${SSG_SOURCE_DIR}/tests/session/test_session_external_modification.cpp
    )
    target_include_directories(test_session_external_modification PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_session_external_modification PRIVATE ssg_core)
    add_test(NAME test_session_external_modification
             COMMAND test_session_external_modification)

    add_executable(test_session_editing
        ${SSG_SOURCE_DIR}/tests/session/test_session_editing.cpp
    )
    target_link_libraries(test_session_editing PRIVATE ssg_tui_objects)
    add_test(NAME test_session_editing COMMAND test_session_editing)

    add_executable(test_session_grid_parity
        ${SSG_SOURCE_DIR}/tests/session/test_session_grid_parity.cpp
    )
    target_link_libraries(test_session_grid_parity PRIVATE ssg_tui_objects)
    target_compile_definitions(test_session_grid_parity PRIVATE
        SSG_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    add_test(NAME test_session_grid_parity COMMAND test_session_grid_parity)

    add_executable(test_session_navigation
        ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
    )
    target_link_libraries(test_session_navigation PRIVATE ssg_tui_objects)
    add_test(NAME test_session_navigation COMMAND test_session_navigation)

    add_executable(test_session_language_services
        ${SSG_SOURCE_DIR}/tests/session/test_session_language_services.cpp
    )
    target_link_libraries(test_session_language_services PRIVATE ssg_core)
    add_test(NAME test_session_language_services COMMAND test_session_language_services)

    add_executable(test_session_totality
        ${SSG_SOURCE_DIR}/tests/session/test_session_totality.cpp
    )
    target_link_libraries(test_session_totality PRIVATE ssg_tui_objects)
    add_test(NAME test_session_totality COMMAND test_session_totality)

    add_executable(test_startup_path
        ${SSG_SOURCE_DIR}/tests/test_startup_path.cpp
    )
    target_include_directories(test_startup_path PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_startup_path PRIVATE ssg_core)
    add_test(NAME test_startup_path COMMAND test_startup_path)

    add_executable(test_command_dispatch
        ${SSG_SOURCE_DIR}/tests/test_command_dispatch.cpp
    )
    target_include_directories(test_command_dispatch PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_command_dispatch PRIVATE ssg_core)
    add_test(NAME test_command_dispatch COMMAND test_command_dispatch)

    add_executable(test_help
        ${SSG_SOURCE_DIR}/tests/session/test_help.cpp
    )
    target_link_libraries(test_help PRIVATE ssg_tui_objects)
    add_test(NAME test_help COMMAND test_help)
endif()
