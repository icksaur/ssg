target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/color.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_color
        ENTRY ${SSG_SOURCE_DIR}/tests/test_color.cpp
        SYMBOL test_color)
    ssg_test_include_directories(test_color PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_color PRIVATE ssg_core)

endif()
