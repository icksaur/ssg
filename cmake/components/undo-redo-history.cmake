target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/DocumentHistory.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_history
        ENTRY ${SSG_SOURCE_DIR}/tests/test_history.cpp
        SYMBOL test_history
        SOURCES
            ${SSG_SOURCE_DIR}/tests/reference_editor.cpp)
    ssg_test_include_directories(test_history PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_history PRIVATE ssg_core)

endif()
