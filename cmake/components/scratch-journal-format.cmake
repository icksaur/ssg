target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ScratchJournal.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_scratch_journal
        ENTRY ${SSG_SOURCE_DIR}/tests/test_scratch_journal.cpp
        SYMBOL test_scratch_journal)
    ssg_test_include_directories(test_scratch_journal PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_scratch_journal PRIVATE
        SSG_SCRATCH_JOURNAL_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/scratch/journal"
    )
    ssg_test_link_libraries(test_scratch_journal PRIVATE ssg_core)

endif()
