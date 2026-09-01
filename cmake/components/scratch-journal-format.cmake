target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ScratchJournal.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_scratch_journal
        ${SSG_SOURCE_DIR}/tests/test_scratch_journal.cpp
    )
    target_include_directories(test_scratch_journal PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_scratch_journal PRIVATE
        SSG_SCRATCH_JOURNAL_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/scratch/journal"
    )
    target_link_libraries(test_scratch_journal PRIVATE ssg_core)
    add_test(NAME test_scratch_journal COMMAND test_scratch_journal)
endif()
