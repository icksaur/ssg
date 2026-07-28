target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/EditorRuntime.cpp
    ${SSG_SOURCE_DIR}/src/runtime/editing.cpp
    ${SSG_SOURCE_DIR}/src/runtime/files.cpp
    ${SSG_SOURCE_DIR}/src/runtime/language_services.cpp
    ${SSG_SOURCE_DIR}/src/runtime/navigation.cpp
    ${SSG_SOURCE_DIR}/src/runtime/presentation.cpp
    ${SSG_SOURCE_DIR}/src/runtime/snapshot.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_runtime_snapshot
        ${SSG_SOURCE_DIR}/tests/runtime/test_runtime_snapshot.cpp
    )
    target_compile_definitions(test_runtime_snapshot PRIVATE
        SSG_SOURCE_SCAN_ROOT="${SSG_SOURCE_DIR}"
    )
    target_include_directories(test_runtime_snapshot PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_runtime_snapshot PRIVATE ssg)
    add_test(NAME test_runtime_snapshot COMMAND test_runtime_snapshot)

    add_executable(test_syntax_injection
        ${SSG_SOURCE_DIR}/tests/test_syntax_injection.cpp
    )
    target_include_directories(test_syntax_injection PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_syntax_injection PRIVATE ssg)
    add_test(NAME test_syntax_injection COMMAND test_syntax_injection)

    add_executable(test_runtime_files
        ${SSG_SOURCE_DIR}/tests/runtime/test_runtime_files.cpp
    )
    target_link_libraries(test_runtime_files PRIVATE ssg)
    add_test(NAME test_runtime_files COMMAND test_runtime_files)

    add_executable(test_runtime_editing
        ${SSG_SOURCE_DIR}/tests/runtime/test_runtime_editing.cpp
    )
    target_link_libraries(test_runtime_editing PRIVATE ssg)
    add_test(NAME test_runtime_editing COMMAND test_runtime_editing)

    add_executable(test_runtime_presentation
        ${SSG_SOURCE_DIR}/tests/runtime/test_runtime_presentation.cpp
    )
    target_link_libraries(test_runtime_presentation PRIVATE ssg)
    add_test(NAME test_runtime_presentation COMMAND test_runtime_presentation)

    add_executable(test_runtime_navigation
        ${SSG_SOURCE_DIR}/tests/runtime/test_runtime_navigation.cpp
    )
    target_link_libraries(test_runtime_navigation PRIVATE ssg)
    add_test(NAME test_runtime_navigation COMMAND test_runtime_navigation)

    add_executable(test_runtime_language_services
        ${SSG_SOURCE_DIR}/tests/runtime/test_runtime_language_services.cpp
    )
    target_link_libraries(test_runtime_language_services PRIVATE ssg)
    add_test(NAME test_runtime_language_services COMMAND test_runtime_language_services)

    add_executable(test_runtime_totality
        ${SSG_SOURCE_DIR}/tests/runtime/test_runtime_totality.cpp
    )
    target_link_libraries(test_runtime_totality PRIVATE ssg)
    add_test(NAME test_runtime_totality COMMAND test_runtime_totality)

    add_executable(test_startup_path
        ${SSG_SOURCE_DIR}/tests/test_startup_path.cpp
    )
    target_include_directories(test_startup_path PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_startup_path PRIVATE ssg)
    add_test(NAME test_startup_path COMMAND test_startup_path)

    add_executable(test_command_dispatch
        ${SSG_SOURCE_DIR}/tests/test_command_dispatch.cpp
    )
    target_include_directories(test_command_dispatch PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_command_dispatch PRIVATE ssg)
    add_test(NAME test_command_dispatch COMMAND test_command_dispatch)
endif()
