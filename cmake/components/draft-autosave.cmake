target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/DraftAutosaveScheduler.cpp
    ${SSG_SOURCE_DIR}/src/DraftReopen.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_draft_autosave
        ${SSG_SOURCE_DIR}/tests/test_draft_autosave.cpp
    )
    target_link_libraries(test_draft_autosave PRIVATE ssg)
    add_test(NAME test_draft_autosave COMMAND test_draft_autosave)

    add_executable(test_draft_reopen
        ${SSG_SOURCE_DIR}/tests/test_draft_reopen.cpp
    )
    target_link_libraries(test_draft_reopen PRIVATE ssg)
    add_test(NAME test_draft_reopen COMMAND test_draft_reopen)
endif()
