# reference-editor component manifest
#
# Manifest contract (see CMakeLists.txt for the full rules):
#   - SSG_SOURCE_DIR is set by CMakeLists.txt to SSG's own source root before
#     this file is included.  Use ${SSG_SOURCE_DIR} for all project-root-
#     relative paths.  Do not use CMAKE_SOURCE_DIR (breaks add_subdirectory).
#   - target ssg must already exist (created before this file is included)
#   - enable_testing() has been called when SSG_SOURCE_DIR == CMAKE_SOURCE_DIR
#   - All target names must be unique across manifests
#
# The reference editor is the independent string-based oracle (spec I13).
# Its sources are NOT added to the ssg library; the oracle shares no editing,
# selection, or history code with SSG.  The aggregate test target compiles
# reference_editor.cpp and test_reference_editor.cpp directly without
# linking to ssg (test_helpers.h is a header-only standalone helper).

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_reference_editor
        ENTRY ${SSG_SOURCE_DIR}/tests/test_reference_editor.cpp
        SYMBOL test_reference_editor
        SOURCES
            ${SSG_SOURCE_DIR}/tests/reference_editor.cpp)
    ssg_test_include_directories(test_reference_editor PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    # No link to ssg: oracle independence is a hard requirement (I13).

endif()
