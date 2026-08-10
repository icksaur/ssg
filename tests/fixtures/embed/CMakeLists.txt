cmake_minimum_required(VERSION 3.20)
project(ssg_embed_consumer CXX)

# Minimal consumer proving SSG can still be embedded with add_subdirectory.
#
# This is the compatibility oracle for the grammar pipeline:
# removing SSG_TREESITTER made the vendored C (tree-sitter plus six grammars) a
# hard build requirement for every consumer, so the embedder path must be proven
# to still configure, build and link rather than assumed.
#
# SSG_EMBED_SOURCE_DIR is supplied by the driving test.
add_subdirectory(${SSG_EMBED_SOURCE_DIR} ${CMAKE_BINARY_DIR}/ssg EXCLUDE_FROM_ALL)

add_executable(embed_consumer main.cpp)
target_link_libraries(embed_consumer PRIVATE ssg)
