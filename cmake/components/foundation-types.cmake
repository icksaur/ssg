# foundation-types component manifest
#
# Manifest contract (see CMakeLists.txt for the full rules):
#   - SSG_SOURCE_DIR is set by CMakeLists.txt to SSG's own source root before
#     this file is included.  Use ${SSG_SOURCE_DIR} for all project-root-
#     relative paths.  Do not use CMAKE_SOURCE_DIR (breaks add_subdirectory).
#   - target ssg must already exist (created before this file is included)
#   - enable_testing() has been called when SSG_SOURCE_DIR == CMAKE_SOURCE_DIR
#   - All target names must be unique across manifests
#
# This manifest adds the config-type implementation to the ssg library and
# registers the standalone type-oracle test executable.

target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/config.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_types
        ${SSG_SOURCE_DIR}/tests/test_types.cpp
    )
    target_link_libraries(test_types PRIVATE ssg)
    add_test(NAME test_types COMMAND test_types)
endif()
