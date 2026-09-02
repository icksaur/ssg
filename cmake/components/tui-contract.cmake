# Milestone 11 — The semantic library API drives the TUI contract.
# Proves the TUI screen is a pure function of the production EditorSession's
# SessionSnapshot: render(snapshot).canonical() == checked-in goldens for the
# normal, prompt, and too-small screens.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_tui_contract
        ${SSG_SOURCE_DIR}/tests/test_tui_contract.cpp
    )
    target_include_directories(test_tui_contract PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_tui_contract PRIVATE ssg_tui_objects)
    add_test(NAME test_tui_contract COMMAND test_tui_contract)
endif()
