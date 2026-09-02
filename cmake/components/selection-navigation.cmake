target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Selection.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_selection
        ENTRY ${SSG_SOURCE_DIR}/tests/test_selection.cpp
        SYMBOL test_selection
        SOURCES
            ${SSG_SOURCE_DIR}/tests/reference_editor.cpp)
    ssg_test_include_directories(test_selection PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/src
    )
    ssg_test_link_libraries(test_selection PRIVATE ssg_core)

endif()
