target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Search.cpp
    ${SSG_SOURCE_DIR}/src/PaletteSearcher.cpp
    ${SSG_SOURCE_DIR}/src/PaletteSubmit.cpp
    ${SSG_SOURCE_DIR}/src/Picker.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_search
        ${SSG_SOURCE_DIR}/tests/test_search.cpp
    )
    target_include_directories(test_search PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_search PRIVATE
        SSG_SEARCH_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/search"
    )
    target_link_libraries(test_search PRIVATE ssg_core)
    add_test(NAME test_search COMMAND test_search)

    add_executable(test_palette
        ${SSG_SOURCE_DIR}/tests/test_palette.cpp
    )
    target_include_directories(test_palette PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_palette PRIVATE ssg_core)
    add_test(NAME test_palette COMMAND test_palette)

    add_executable(test_palette_submit
        ${SSG_SOURCE_DIR}/tests/test_palette_submit.cpp
    )
    target_include_directories(test_palette_submit PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_palette_submit PRIVATE ssg_core)
    add_test(NAME test_palette_submit COMMAND test_palette_submit)

    add_executable(test_palette_host_ranking
        ${SSG_SOURCE_DIR}/tests/test_palette_host_ranking.cpp
    )
    target_include_directories(test_palette_host_ranking PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_palette_host_ranking PRIVATE ssg_core)
    add_test(NAME test_palette_host_ranking COMMAND test_palette_host_ranking)

    add_executable(test_picker
        ${SSG_SOURCE_DIR}/tests/test_picker.cpp
    )
    target_include_directories(test_picker PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_picker PRIVATE ssg_core)
    add_test(NAME test_picker COMMAND test_picker)

    add_executable(test_fuzzy_corpus
        ${SSG_SOURCE_DIR}/tests/test_fuzzy_corpus.cpp
    )
    target_include_directories(test_fuzzy_corpus PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_fuzzy_corpus PRIVATE
        SSG_FUZZY_CORPUS="${SSG_SOURCE_DIR}/tests/fixtures/fuzzy_corpus.tsv"
    )
    target_link_libraries(test_fuzzy_corpus PRIVATE ssg_core)
    add_test(NAME test_fuzzy_corpus COMMAND test_fuzzy_corpus)
endif()
