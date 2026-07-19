# Milestone 11 — Library API is the contract (doc/spec-library-contract.md).
# Proves the TUI screen is a pure function of the production EditorRuntime's
# SessionSnapshot: render(snapshot).canonical() == checked-in goldens for the
# normal, prompt, and too-small screens.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_library_contract
        ${SSG_SOURCE_DIR}/tests/test_library_contract.cpp
    )
    target_include_directories(test_library_contract PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_compile_definitions(test_library_contract PRIVATE
        SSG_CONTRACT_NORMAL_GOLDEN="${SSG_SOURCE_DIR}/tests/fixtures/tui/runtime-normal.txt"
        SSG_CONTRACT_PALETTE_GOLDEN="${SSG_SOURCE_DIR}/tests/fixtures/tui/runtime-palette.txt"
        SSG_CONTRACT_TOO_SMALL_GOLDEN="${SSG_SOURCE_DIR}/tests/fixtures/tui/runtime-too-small.txt"
    )
    target_link_libraries(test_library_contract PRIVATE ssg)
    add_test(NAME test_library_contract COMMAND test_library_contract)
endif()
