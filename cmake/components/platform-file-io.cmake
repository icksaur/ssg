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
    ssg_add_test_suite(
        NAME test_platform_files
        ENTRY ${SSG_SOURCE_DIR}/tests/test_platform_files.cpp
        SYMBOL test_platform_files)
    ssg_test_include_directories(test_platform_files PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_platform_files PRIVATE ssg_platform)

    ssg_add_test_suite(
        NAME test_platform_file_seam
        ENTRY ${SSG_SOURCE_DIR}/tests/test_platform_file_seam.cpp
        SYMBOL test_platform_file_seam)
    ssg_test_include_directories(test_platform_file_seam PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_platform_file_seam PRIVATE ssg_platform)

    ssg_add_test_suite(
        NAME test_platform_interface
        ENTRY ${SSG_SOURCE_DIR}/tests/test_platform_interface.cpp
        SYMBOL test_platform_interface)
    ssg_test_link_libraries(test_platform_interface PRIVATE ssg_platform)

    ssg_add_test_suite(
        NAME test_file_seam_guard
        ENTRY ${SSG_SOURCE_DIR}/tests/test_file_seam_guard.cpp
        SYMBOL test_file_seam_guard)
    ssg_test_link_libraries(test_file_seam_guard PRIVATE ssg_platform)
    set_tests_properties(test_file_seam_guard PROPERTIES
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
