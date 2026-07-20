target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/Search.cpp
    ${SSG_SOURCE_DIR}/src/PaletteSearcher.cpp
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
    target_link_libraries(test_search PRIVATE ssg)
    add_test(NAME test_search COMMAND test_search)

    add_executable(test_palette
        ${SSG_SOURCE_DIR}/tests/test_palette.cpp
    )
    target_include_directories(test_palette PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_palette PRIVATE ssg)
    add_test(NAME test_palette COMMAND test_palette)
endif()
