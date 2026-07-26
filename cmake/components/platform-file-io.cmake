target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/platform_files.cpp
)

if(WIN32)
    target_sources(ssg PRIVATE
        ${SSG_SOURCE_DIR}/src/platform/windows_files.cpp
    )
    target_link_libraries(ssg PRIVATE advapi32)
else()
    target_sources(ssg PRIVATE
        ${SSG_SOURCE_DIR}/src/platform/linux_files.cpp
    )
endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_platform_files
        ${SSG_SOURCE_DIR}/tests/test_platform_files.cpp
    )
    target_include_directories(test_platform_files PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_platform_files PRIVATE ssg)
    add_test(NAME test_platform_files COMMAND test_platform_files)

    add_executable(test_platform_file_seam
        ${SSG_SOURCE_DIR}/tests/test_platform_file_seam.cpp
    )
    target_include_directories(test_platform_file_seam PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_platform_file_seam PRIVATE ssg)
    add_test(NAME test_platform_file_seam COMMAND test_platform_file_seam)

    add_executable(test_file_seam_guard
        ${SSG_SOURCE_DIR}/tests/test_file_seam_guard.cpp
    )
    target_link_libraries(test_file_seam_guard PRIVATE ssg)
    add_test(NAME test_file_seam_guard COMMAND test_file_seam_guard
        WORKING_DIRECTORY ${SSG_SOURCE_DIR})
endif()
