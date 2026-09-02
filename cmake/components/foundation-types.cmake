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
# registers the type-oracle suite.

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/config.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_types
        ENTRY ${SSG_SOURCE_DIR}/tests/test_types.cpp
        SYMBOL test_types)
    ssg_test_link_libraries(test_types PRIVATE ssg_core)

endif()
