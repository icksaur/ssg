target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Settings.cpp
)
target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/platform/linux_settings.cpp
    ${SSG_SOURCE_DIR}/src/platform/windows_settings.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_settings
        ${SSG_SOURCE_DIR}/tests/test_settings.cpp
    )
    target_include_directories(test_settings PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_settings PRIVATE ssg_core)
    add_test(NAME test_settings COMMAND test_settings)

    add_executable(test_settings_persistence
        ${SSG_SOURCE_DIR}/tests/test_settings_persistence.cpp
    )
    target_include_directories(test_settings_persistence PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_settings_persistence PRIVATE ssg_core)
    add_test(NAME test_settings_persistence COMMAND test_settings_persistence)
endif()
