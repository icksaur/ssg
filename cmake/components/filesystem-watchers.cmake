target_sources(ssg_platform PRIVATE
    ${SSG_SOURCE_DIR}/src/FilesystemWatcher.cpp
)

if(WIN32)
    target_sources(ssg_platform PRIVATE
        ${SSG_SOURCE_DIR}/src/platform/windows_watcher.cpp
        ${SSG_SOURCE_DIR}/src/platform/windows_git_metadata_watcher.cpp
    )
else()
    target_sources(ssg_platform PRIVATE
        ${SSG_SOURCE_DIR}/src/platform/linux_watcher.cpp
        ${SSG_SOURCE_DIR}/src/platform/linux_git_metadata_watcher.cpp
    )
endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_watcher_tests
        ${SSG_SOURCE_DIR}/tests/test_watcher.cpp
    )
    target_link_libraries(ssg_watcher_tests PRIVATE ssg_platform)
    add_test(NAME ssg_watcher_tests COMMAND ssg_watcher_tests)
endif()
