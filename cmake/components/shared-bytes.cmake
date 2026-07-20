# shared-bytes component manifest (Milestone 13, doc/spec-large-files-loading.md)
#
# The immutable ref-counted byte-buffer handle (LF-2) shared by the decoded text,
# the piece-tree original, and the initial persisted_text (wired in LF-3b).

target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/shared_bytes.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_shared_bytes
        ${SSG_SOURCE_DIR}/tests/test_shared_bytes.cpp
    )
    target_link_libraries(test_shared_bytes PRIVATE ssg)
    add_test(NAME test_shared_bytes COMMAND test_shared_bytes)
endif()
