target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/StatusFields.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_status_fields
        ENTRY ${SSG_SOURCE_DIR}/tests/test_status_fields.cpp
        SYMBOL test_status_fields)
    ssg_test_include_directories(test_status_fields PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_status_fields PRIVATE ssg_tui_objects)

endif()
