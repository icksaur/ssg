# Milestone 11 — Library API is the contract.
# Proves the TUI screen is a pure function of the production EditorSession's
# SessionSnapshot: render(snapshot).canonical() == checked-in goldens for the
# normal, prompt, and too-small screens.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_library_contract
        ${SSG_SOURCE_DIR}/tests/test_library_contract.cpp
    )
    target_include_directories(test_library_contract PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_library_contract PRIVATE ssg_grid)
    add_test(NAME test_library_contract COMMAND test_library_contract)
endif()
