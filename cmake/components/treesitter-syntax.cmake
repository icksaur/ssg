target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/SyntaxModel.cpp
)

# Tree-sitter is compiled unconditionally.  Highlighting is disabled at RUNTIME
# by constructing the runtime with a null EditorRuntimeConfig::syntaxParser,
# which yields plain text; see doc/spec-grammar-pipeline.md.
enable_language(C)

set(_SSG_TREESITTER_VENDOR_DIR ${SSG_SOURCE_DIR}/vendor)

# Highlight queries embedded into the binary.  ONE LINE PER QUERY: add a
# "key=path" entry here and it is compiled in; nothing else in the build
# needs to change.  The keys are what src/TreeSitterParser.cpp's kGrammars
# table refers to.
set(_SSG_EMBEDDED_QUERIES
    "c=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c/queries/highlights.scm"
    "cpp=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-cpp/queries/highlights.scm"
    "javascript=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-javascript/queries/highlights.scm"
    "typescript=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-typescript/queries/highlights.scm"
    "csharp=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c-sharp/queries/highlights.scm"
    "lua=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-lua/queries/highlights.scm"
    "markdown=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown/queries/highlights.scm"
    "markdown_inline=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown-inline/queries/highlights.scm"
)

# Depend on the query files themselves so editing one regenerates the TU.
set(_SSG_EMBEDDED_QUERY_FILES "")
foreach(_entry IN LISTS _SSG_EMBEDDED_QUERIES)
    string(FIND "${_entry}" "=" _split)
    math(EXPR _rest "${_split} + 1")
    string(SUBSTRING "${_entry}" ${_rest} -1 _path)
    list(APPEND _SSG_EMBEDDED_QUERY_FILES "${_path}")
endforeach()

set(_SSG_EMBEDDED_QUERIES_TU
    ${CMAKE_BINARY_DIR}/generated/treesitter_queries.cpp)
# The spec goes through a FILE rather than a -D argument.  Neither a
# ';'-separated list (truncated at the first ';' by the command-line parser)
# nor a newline-joined string (newlines stripped under VERBATIM) survives
# transit intact, and both failure modes are silent -- they yield a partial
# table that still compiles.
set(_SSG_EMBEDDED_QUERIES_SPEC
    ${CMAKE_BINARY_DIR}/generated/treesitter_queries.spec)
string(JOIN "\n" _SSG_EMBEDDED_QUERIES_TEXT ${_SSG_EMBEDDED_QUERIES})
file(GENERATE OUTPUT ${_SSG_EMBEDDED_QUERIES_SPEC}
     CONTENT "${_SSG_EMBEDDED_QUERIES_TEXT}\n")
add_custom_command(
    OUTPUT ${_SSG_EMBEDDED_QUERIES_TU}
    COMMAND ${CMAKE_COMMAND}
        -DEMBED_SPEC_FILE=${_SSG_EMBEDDED_QUERIES_SPEC}
        -DEMBED_OUTPUT=${_SSG_EMBEDDED_QUERIES_TU}
        -DEMBED_NAMESPACE=ssg
        -DEMBED_ACCESSOR=embeddedHighlightQuery
        -P ${SSG_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS ${_SSG_EMBEDDED_QUERY_FILES}
            ${_SSG_EMBEDDED_QUERIES_SPEC}
            ${SSG_SOURCE_DIR}/cmake/embed_text.cmake
    COMMENT "Embedding tree-sitter highlight queries"
    VERBATIM)

set(_SSG_TREESITTER_VENDOR_SOURCES
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter/lib/src/lib.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-cpp/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-cpp/src/scanner.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-javascript/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-javascript/src/scanner.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-typescript/typescript/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-typescript/typescript/src/scanner.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c-sharp/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c-sharp/src/scanner.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-lua/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-lua/src/scanner.c
    # Only the BLOCK grammar is vendored.  Upstream splits markdown in two, and
    # the inline grammar (emphasis, links, code spans) can only be applied
    # through a language injection, which this parser does not implement.
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown/src/scanner.c
    # The inline half, run as a language injection inside the block grammar's
    # `inline` nodes (emphasis, code spans, inline links).
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown-inline/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown-inline/src/scanner.c
)

target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/TreeSitterParser.cpp
    ${_SSG_EMBEDDED_QUERIES_TU}
    ${_SSG_TREESITTER_VENDOR_SOURCES}
)

target_include_directories(ssg PRIVATE
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter/lib/include
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-cpp/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-javascript/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-typescript/typescript/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c-sharp/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-lua/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown-inline/src
)

set_source_files_properties(${_SSG_TREESITTER_VENDOR_SOURCES}
    PROPERTIES
        COMPILE_OPTIONS "-w"
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_syntax
        ${SSG_SOURCE_DIR}/tests/test_syntax.cpp
    )
    target_include_directories(test_syntax PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_syntax PRIVATE ssg)
    add_test(NAME test_syntax COMMAND test_syntax)

    add_executable(test_syntax_language_detection
        ${SSG_SOURCE_DIR}/tests/test_syntax_language_detection.cpp
    )
    target_include_directories(test_syntax_language_detection PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_syntax_language_detection PRIVATE ssg)
    add_test(NAME test_syntax_language_detection COMMAND test_syntax_language_detection)

    add_executable(test_treesitter_syntax
        ${SSG_SOURCE_DIR}/tests/test_treesitter_syntax.cpp
    )
    target_include_directories(test_treesitter_syntax PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/src
    )
    target_compile_definitions(test_treesitter_syntax PRIVATE
        SSG_TREESITTER_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/syntax"
        SSG_TREESITTER_VENDOR_DIR="${_SSG_TREESITTER_VENDOR_DIR}"
        SSG_TREESITTER_SOURCE_DIR="${SSG_SOURCE_DIR}/src"
    )
    target_link_libraries(test_treesitter_syntax PRIVATE ssg)
    add_test(NAME test_treesitter_syntax COMMAND test_treesitter_syntax)

    # A separate executable so no earlier test has compiled a query first,
    # which would make these checks vacuous.
    add_executable(test_treesitter_embedded_queries
        ${SSG_SOURCE_DIR}/tests/test_treesitter_embedded_queries.cpp
    )
    target_include_directories(test_treesitter_embedded_queries PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/src
    )
    target_compile_definitions(test_treesitter_embedded_queries PRIVATE
        SSG_TREESITTER_VENDOR_DIR="${_SSG_TREESITTER_VENDOR_DIR}"
    )
    target_link_libraries(test_treesitter_embedded_queries PRIVATE ssg)
    add_test(NAME test_treesitter_embedded_queries
             COMMAND test_treesitter_embedded_queries)
    set_tests_properties(test_treesitter_embedded_queries PROPERTIES
        RUN_SERIAL TRUE)

    # The public registration seam.  Deliberately does NOT get the vendored
    # include directories: it must compile against <ssg/TreeSitterGrammars.h>
    # alone, which is what proves a host is not forced to have tree-sitter's
    # headers.
    add_executable(test_treesitter_grammar_registration
        ${SSG_SOURCE_DIR}/tests/test_treesitter_grammar_registration.cpp
    )
    target_include_directories(test_treesitter_grammar_registration PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_treesitter_grammar_registration PRIVATE ssg)
    add_test(NAME test_treesitter_grammar_registration
             COMMAND test_treesitter_grammar_registration)
endif()
