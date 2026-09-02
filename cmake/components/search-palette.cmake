target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Search.cpp
    ${SSG_SOURCE_DIR}/src/PaletteSearcher.cpp
    ${SSG_SOURCE_DIR}/src/PaletteSubmit.cpp
    ${SSG_SOURCE_DIR}/src/Picker.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_search
        ENTRY ${SSG_SOURCE_DIR}/tests/test_search.cpp
        SYMBOL test_search)
    ssg_test_include_directories(test_search PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_search PRIVATE
        SSG_SEARCH_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/search"
    )
    ssg_test_link_libraries(test_search PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_palette
        ENTRY ${SSG_SOURCE_DIR}/tests/test_palette.cpp
        SYMBOL test_palette)
    ssg_test_include_directories(test_palette PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_palette PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_palette_submit
        ENTRY ${SSG_SOURCE_DIR}/tests/test_palette_submit.cpp
        SYMBOL test_palette_submit)
    ssg_test_include_directories(test_palette_submit PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_palette_submit PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_palette_host_ranking
        ENTRY ${SSG_SOURCE_DIR}/tests/test_palette_host_ranking.cpp
        SYMBOL test_palette_host_ranking)
    ssg_test_include_directories(test_palette_host_ranking PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_palette_host_ranking PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_picker
        ENTRY ${SSG_SOURCE_DIR}/tests/test_picker.cpp
        SYMBOL test_picker)
    ssg_test_include_directories(test_picker PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_picker PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_fuzzy_corpus
        ENTRY ${SSG_SOURCE_DIR}/tests/test_fuzzy_corpus.cpp
        SYMBOL test_fuzzy_corpus)
    ssg_test_include_directories(test_fuzzy_corpus PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_fuzzy_corpus PRIVATE
        SSG_FUZZY_CORPUS="${SSG_SOURCE_DIR}/tests/fixtures/fuzzy_corpus.tsv"
    )
    ssg_test_link_libraries(test_fuzzy_corpus PRIVATE ssg_core)

endif()
