# Milestone 11 — M11-2: the app is transport only (doc/spec-library-contract.md,
# INV-app-transport-only).
#
# An independent ANSI decoder (round-trip self-tested) decodes the REAL `ssg`
# binary's pty output and asserts the decoded screen equals render(snapshot) —
# text, resolved color, and cursor.  The pty capture is Linux-scoped (forkpty);
# the decoder and comparison are portable.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    if(UNIX AND NOT APPLE)
        add_executable(test_terminal_parity
            ${SSG_SOURCE_DIR}/tests/test_terminal_parity.cpp
            ${SSG_SOURCE_DIR}/apps/ssg_terminal.cpp
            ${SSG_SOURCE_DIR}/apps/pointer_routing.cpp
        )
        target_include_directories(test_terminal_parity PRIVATE
            ${SSG_SOURCE_DIR}/apps
            ${SSG_SOURCE_DIR}/tests
        )
        target_link_libraries(test_terminal_parity PRIVATE ssg util)
        add_dependencies(test_terminal_parity ssg_app)
        target_compile_definitions(test_terminal_parity PRIVATE
            SSG_APP_BINARY="$<TARGET_FILE:ssg_app>"
        )
        add_test(NAME test_terminal_parity COMMAND test_terminal_parity)
        set_tests_properties(test_terminal_parity PROPERTIES
            LABELS "startup"
            TIMEOUT 60)
    endif()
endif()
