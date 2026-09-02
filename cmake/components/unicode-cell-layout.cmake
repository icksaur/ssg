# unicode-cell-layout component manifest
#
# Manifest contract (see CMakeLists.txt for the full rules):
#   - SSG_SOURCE_DIR is set by CMakeLists.txt to SSG's own source root before
#     this file is included.  Use ${SSG_SOURCE_DIR} for all project-root-
#     relative paths.  Do not use CMAKE_SOURCE_DIR (breaks add_subdirectory).
#   - target ssg must already exist (created before this file is included)
#   - enable_testing() has been called when SSG_SOURCE_DIR == CMAKE_SOURCE_DIR
#   - All target names must be unique across manifests
#
# This manifest adds the UTF-8 grapheme segmentation and terminal cell-layout
# implementation to the ssg library and registers the cell-layout oracle suite.

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/GraphemeLayout.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_cell_layout
        ENTRY ${SSG_SOURCE_DIR}/tests/test_cell_layout.cpp
        SYMBOL test_cell_layout)
    ssg_test_link_libraries(test_cell_layout PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_gcb_oracle
        ENTRY ${SSG_SOURCE_DIR}/tests/test_gcb_oracle.cpp
        SYMBOL test_gcb_oracle)
    ssg_test_link_libraries(test_gcb_oracle PRIVATE ssg_core)
    ssg_test_compile_definitions(test_gcb_oracle PRIVATE
        UNICODE_DATA_DIR="${SSG_SOURCE_DIR}/data/unicode"
    )

endif()
