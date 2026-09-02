# Milestone 11 — The semantic library API drives the TUI contract.
# Proves the TUI screen is a pure function of the production EditorSession's
# SessionSnapshot: render(snapshot).canonical() == checked-in goldens for the
# normal, prompt, and too-small screens.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_tui_contract
        ENTRY ${SSG_SOURCE_DIR}/tests/test_tui_contract.cpp
        SYMBOL test_tui_contract)
    ssg_test_include_directories(test_tui_contract PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_tui_contract PRIVATE ssg_tui_objects)

endif()
