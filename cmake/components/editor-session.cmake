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
    ssg_add_test_suite(
        NAME test_session_snapshot
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_snapshot.cpp
        SYMBOL test_session_snapshot)
    ssg_test_compile_definitions(test_session_snapshot PRIVATE
        SSG_SOURCE_SCAN_ROOT="${SSG_SOURCE_DIR}"
    )
    ssg_test_include_directories(test_session_snapshot PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_session_snapshot PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_syntax_injection
        ENTRY ${SSG_SOURCE_DIR}/tests/test_syntax_injection.cpp
        SYMBOL test_syntax_injection)
    ssg_test_include_directories(test_syntax_injection PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_syntax_injection PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_session_files
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
        SYMBOL test_session_files)
    ssg_test_link_libraries(test_session_files PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_reopen
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
        SYMBOL test_session_reopen)
    ssg_test_link_libraries(test_session_reopen PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_drafts
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
        SYMBOL test_session_drafts)
    ssg_test_link_libraries(test_session_drafts PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_notices
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
        SYMBOL test_session_notices)
    ssg_test_link_libraries(test_session_notices PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_conflicts
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
        SYMBOL test_session_conflicts)
    ssg_test_link_libraries(test_session_conflicts PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_session_external_modification
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_external_modification.cpp
        SYMBOL test_session_external_modification)
    ssg_test_include_directories(test_session_external_modification PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_session_external_modification PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_session_editing
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_editing.cpp
        SYMBOL test_session_editing)
    ssg_test_link_libraries(test_session_editing PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_session_grid_parity
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_grid_parity.cpp
        SYMBOL test_session_grid_parity)
    ssg_test_link_libraries(test_session_grid_parity PRIVATE ssg_tui_objects)
    ssg_test_compile_definitions(test_session_grid_parity PRIVATE
        SSG_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )

    ssg_add_test_suite(
        NAME test_session_navigation
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
        SYMBOL test_session_navigation)
    ssg_test_include_directories(test_session_navigation PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_session_navigation PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_follow
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
        SYMBOL test_session_follow)
    ssg_test_include_directories(test_session_follow PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_session_follow PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_pickers
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
        SYMBOL test_session_pickers)
    ssg_test_include_directories(test_session_pickers PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_session_pickers PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_interaction
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
        SYMBOL test_session_interaction)
    ssg_test_include_directories(test_session_interaction PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_session_interaction PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_layout
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
        SYMBOL test_session_layout)
    ssg_test_include_directories(test_session_layout PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_session_layout PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_session_language_services
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_language_services.cpp
        SYMBOL test_session_language_services)
    ssg_test_link_libraries(test_session_language_services PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_session_totality
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_totality.cpp
        SYMBOL test_session_totality)
    ssg_test_link_libraries(test_session_totality PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_startup_path
        ENTRY ${SSG_SOURCE_DIR}/tests/test_startup_path.cpp
        SYMBOL test_startup_path)
    ssg_test_include_directories(test_startup_path PRIVATE
        ${SSG_SOURCE_DIR}/src
        ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_startup_path PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_command_dispatch
        ENTRY ${SSG_SOURCE_DIR}/tests/test_command_dispatch.cpp
        SYMBOL test_command_dispatch)
    ssg_test_include_directories(test_command_dispatch PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_command_dispatch PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_help
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_help.cpp
        SYMBOL test_help)
    ssg_test_link_libraries(test_help PRIVATE ssg_tui_objects)

endif()
