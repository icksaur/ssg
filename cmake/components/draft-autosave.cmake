target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/DraftAutosaveScheduler.cpp
    ${SSG_SOURCE_DIR}/src/DraftReopenClassifier.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_draft_autosave
        ${SSG_SOURCE_DIR}/tests/test_draft_autosave.cpp
    )
    target_link_libraries(test_draft_autosave PRIVATE ssg_core)
    add_test(NAME test_draft_autosave COMMAND test_draft_autosave)

    add_executable(test_draft_reopen
        ${SSG_SOURCE_DIR}/tests/test_draft_reopen.cpp
    )
    target_link_libraries(test_draft_reopen PRIVATE ssg_core)
    add_test(NAME test_draft_reopen COMMAND test_draft_reopen)
endif()
