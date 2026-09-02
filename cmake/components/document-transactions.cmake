target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Document.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_document
        ENTRY ${SSG_SOURCE_DIR}/tests/test_document.cpp
        SYMBOL test_document
        SOURCES
            ${SSG_SOURCE_DIR}/tests/reference_editor.cpp)
    ssg_test_include_directories(test_document PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_document PRIVATE ssg_core)

endif()
