target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/DraftAutosaveScheduler.cpp
    ${SSG_SOURCE_DIR}/src/DraftReopenClassifier.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_draft_autosave
        ENTRY ${SSG_SOURCE_DIR}/tests/test_draft_autosave.cpp
        SYMBOL test_draft_autosave)
    ssg_test_link_libraries(test_draft_autosave PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_draft_reopen
        ENTRY ${SSG_SOURCE_DIR}/tests/test_draft_reopen.cpp
        SYMBOL test_draft_reopen)
    ssg_test_link_libraries(test_draft_reopen PRIVATE ssg_core)

endif()
