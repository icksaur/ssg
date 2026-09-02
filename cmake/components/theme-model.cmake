target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Theme.cpp
    ${SSG_SOURCE_DIR}/src/DefaultTheme.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_theme
        ENTRY ${SSG_SOURCE_DIR}/tests/test_theme.cpp
        SYMBOL test_theme)
    ssg_test_include_directories(test_theme PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_theme PRIVATE
        SSG_SOURCE_ROOT="${SSG_SOURCE_DIR}"
        SSG_THEME_ROLES_PATH="${SSG_SOURCE_DIR}/tests/fixtures/theme_roles.json"
    )
    ssg_test_link_libraries(test_theme PRIVATE ssg_core)

endif()
