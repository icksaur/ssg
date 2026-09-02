target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Settings.cpp
)
target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/platform/linux_settings.cpp
    ${SSG_SOURCE_DIR}/src/platform/windows_settings.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_settings
        ENTRY ${SSG_SOURCE_DIR}/tests/test_settings.cpp
        SYMBOL test_settings)
    ssg_test_include_directories(test_settings PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_settings PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_settings_persistence
        ENTRY ${SSG_SOURCE_DIR}/tests/test_settings_persistence.cpp
        SYMBOL test_settings_persistence)
    ssg_test_include_directories(test_settings_persistence PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_settings_persistence PRIVATE ssg_core)

endif()
