# foundation-types component manifest
#
# Manifest contract (see CMakeLists.txt for the full rules):
#   - target ssg must already exist (created before this file is included)
#   - enable_testing() must have been called (called before the glob loop)
#   - Use ${CMAKE_SOURCE_DIR} for project-root-relative paths; do not rely on
#     CMAKE_CURRENT_LIST_DIR for source or test paths
#   - All target names must be unique across manifests
#
# This manifest adds the config-type implementation to the ssg library and
# registers the standalone type-oracle test executable.

target_sources(ssg PRIVATE
    ${CMAKE_SOURCE_DIR}/src/config.cpp
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(test_types
        ${CMAKE_SOURCE_DIR}/tests/test_types.cpp
    )
    target_link_libraries(test_types PRIVATE ssg)
    add_test(NAME test_types COMMAND test_types)
endif()
