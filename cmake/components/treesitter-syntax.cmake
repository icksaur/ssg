target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/SyntaxModel.cpp
)

if(SSG_TREESITTER)
    enable_language(C)

    set(_SSG_TREESITTER_VENDOR_DIR ${SSG_SOURCE_DIR}/vendor)

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
    )

    target_sources(ssg PRIVATE
        ${SSG_SOURCE_DIR}/src/TreeSitterParser.cpp
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
    )

    target_compile_definitions(ssg PRIVATE
        SSG_TREESITTER
        SSG_TREESITTER_VENDOR_DIR="${_SSG_TREESITTER_VENDOR_DIR}"
    )

    set_source_files_properties(${_SSG_TREESITTER_VENDOR_SOURCES}
        PROPERTIES
            COMPILE_OPTIONS "-w"
    )
endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_syntax
        ${SSG_SOURCE_DIR}/tests/test_syntax.cpp
    )
    target_include_directories(test_syntax PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_syntax PRIVATE ssg)
    add_test(NAME test_syntax COMMAND test_syntax)

    if(SSG_TREESITTER)
        add_executable(test_treesitter_syntax
            ${SSG_SOURCE_DIR}/tests/test_treesitter_syntax.cpp
        )
        target_include_directories(test_treesitter_syntax PRIVATE
            ${SSG_SOURCE_DIR}/tests
            ${SSG_SOURCE_DIR}/src
        )
        target_compile_definitions(test_treesitter_syntax PRIVATE
            SSG_TREESITTER
            SSG_TREESITTER_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/syntax"
        )
        target_link_libraries(test_treesitter_syntax PRIVATE ssg)
        add_test(NAME test_treesitter_syntax COMMAND test_treesitter_syntax)
    endif()
endif()
