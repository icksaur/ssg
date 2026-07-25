# Compatibility oracle for doc/spec-grammar-pipeline.md Phase B.
#
# Removing SSG_TREESITTER made the vendored tree-sitter C sources a hard build
# requirement for EVERY consumer, including projects that pull SSG in with
# add_subdirectory.  That path is not otherwise exercised -- SSG's own build is
# always the top-level project -- so it is proven here rather than assumed.
#
# Configures and builds a minimal consumer project in a scratch directory.  Not
# a unit test: it drives CMake, so it is registered as its own ctest entry and
# marked expensive.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_test(NAME test_embed_consumer
        COMMAND ${CMAKE_COMMAND}
            -DSSG_EMBED_SOURCE_DIR=${SSG_SOURCE_DIR}
            -DSSG_EMBED_FIXTURE_DIR=${SSG_SOURCE_DIR}/tests/fixtures/embed
            -DSSG_EMBED_WORK_DIR=${CMAKE_BINARY_DIR}/embed-consumer
            -DSSG_EMBED_GENERATOR=${CMAKE_GENERATOR}
            -P ${SSG_SOURCE_DIR}/cmake/run_embed_consumer.cmake)
    # A full library build; keep it off the parallel critical path.
    set_tests_properties(test_embed_consumer PROPERTIES
        LABELS "embed"
        TIMEOUT 900)
endif()
