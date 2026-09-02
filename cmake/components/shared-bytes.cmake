# shared-bytes component manifest
#
# The immutable ref-counted byte-buffer handle (LF-2) shared by the decoded text,
# the piece-tree original, and the initial persisted_text (wired in LF-3b).

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/SharedBytes.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_shared_bytes
        ENTRY ${SSG_SOURCE_DIR}/tests/test_shared_bytes.cpp
        SYMBOL test_shared_bytes)
    ssg_test_link_libraries(test_shared_bytes PRIVATE ssg_core)

endif()
