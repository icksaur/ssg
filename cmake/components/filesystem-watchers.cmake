target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/FilesystemWatcher.cpp
)

if(WIN32)
    target_sources(ssg PRIVATE
        ${SSG_SOURCE_DIR}/src/platform/windows_watcher.cpp
    )
else()
    target_sources(ssg PRIVATE
        ${SSG_SOURCE_DIR}/src/platform/linux_watcher.cpp
    )
endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_watcher_tests
        ${SSG_SOURCE_DIR}/tests/test_watcher.cpp
    )
    target_link_libraries(ssg_watcher_tests PRIVATE ssg)
    add_test(NAME ssg_watcher_tests COMMAND ssg_watcher_tests)
endif()
