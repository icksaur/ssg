target_sources(ssg_platform PRIVATE
    ${SSG_SOURCE_DIR}/src/platform_files.cpp
)

if(WIN32)
    target_sources(ssg_platform PRIVATE
        ${SSG_SOURCE_DIR}/src/platform/windows_files.cpp
    )
    target_link_libraries(ssg_platform PRIVATE advapi32)
else()
    target_sources(ssg_platform PRIVATE
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
    target_link_libraries(test_platform_files PRIVATE ssg_platform)
    add_test(NAME test_platform_files COMMAND test_platform_files)

    add_executable(test_platform_file_seam
        ${SSG_SOURCE_DIR}/tests/test_platform_file_seam.cpp
    )
    target_include_directories(test_platform_file_seam PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_platform_file_seam PRIVATE ssg_platform)
    add_test(NAME test_platform_file_seam COMMAND test_platform_file_seam)

    add_executable(test_platform_interface
        ${SSG_SOURCE_DIR}/tests/test_platform_interface.cpp
    )
    target_link_libraries(test_platform_interface PRIVATE ssg_platform)
    add_test(NAME test_platform_interface COMMAND test_platform_interface)

    add_executable(test_file_seam_guard
        ${SSG_SOURCE_DIR}/tests/test_file_seam_guard.cpp
    )
    target_link_libraries(test_file_seam_guard PRIVATE ssg_platform)
    add_test(NAME test_file_seam_guard COMMAND test_file_seam_guard
        WORKING_DIRECTORY ${SSG_SOURCE_DIR})

    foreach(_mode edge root)
        add_test(NAME test_layer_graph_${_mode}
            COMMAND ${CMAKE_COMMAND}
                -DSSG_SOURCE_DIR=${SSG_SOURCE_DIR}
                -DSSG_LAYER_FIXTURE_DIR=${SSG_SOURCE_DIR}/tests/fixtures/layer_graph
                -DSSG_LAYER_WORK_DIR=${CMAKE_BINARY_DIR}/layer-graph-${_mode}
                -DSSG_LAYER_GENERATOR=${CMAKE_GENERATOR}
                -DSSG_LAYER_FIXTURE_MODE=${_mode}
                -P ${SSG_SOURCE_DIR}/cmake/run_layer_graph_fixture.cmake)
    endforeach()
endif()
