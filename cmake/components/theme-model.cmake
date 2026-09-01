target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Theme.cpp
    ${SSG_SOURCE_DIR}/src/DefaultTheme.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_theme
        ${SSG_SOURCE_DIR}/tests/test_theme.cpp
    )
    target_include_directories(test_theme PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_theme PRIVATE
        SSG_SOURCE_ROOT="${SSG_SOURCE_DIR}"
        SSG_THEME_ROLES_PATH="${SSG_SOURCE_DIR}/tests/fixtures/theme_roles.json"
    )
    target_link_libraries(test_theme PRIVATE ssg_core)
    add_test(NAME test_theme COMMAND test_theme)
endif()
