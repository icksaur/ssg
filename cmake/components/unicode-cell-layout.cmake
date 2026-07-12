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
# implementation to the ssg library and registers the standalone cell-layout
# oracle test executable.

target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/layout.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_cell_layout
        ${SSG_SOURCE_DIR}/tests/test_cell_layout.cpp
    )
    target_link_libraries(test_cell_layout PRIVATE ssg)
    add_test(NAME test_cell_layout COMMAND test_cell_layout)
endif()
