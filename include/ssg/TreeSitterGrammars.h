#pragma once

#include <ssg/SyntaxModel.h>

#include <memory>
#include <string>
#include <vector>

namespace ssg {

// An opaque tree-sitter language pointer.
//
// Deliberately `const void*` rather than `const TSLanguage*`: this is a PUBLIC
// header, and typing it concretely would force every consumer of it to have
// tree-sitter's headers on their include path, including consumers that never
// register a grammar. A host that is registering one is already linking that
// grammar's generated parser and therefore already has the real pointer -- it
// passes the `const TSLanguage*` returned by tree-sitter's
// `tree_sitter_<name>()` entry point, which converts implicitly.
//
// The trade is real: this gives up compile-time type checking on that one
// argument in exchange for a public header that pulls in no third-party types.
using SyntaxLanguageHandle = const void*;

// Returns the grammar's language pointer. A function rather than a value
// because tree-sitter's entry points are functions, and calling one at
// registration time would construct a grammar before the first frame, which
// the startup budget forbids (doc/spec-fast-startup.md INV-no-optional-init).
using SyntaxLanguageFactory = SyntaxLanguageHandle (*)();

// One registerable grammar: how to name it, how to get it, and how to color it.
struct TreeSitterGrammar {
    // Every id this grammar answers to; the first is conventionally canonical.
    // A vector rather than a fixed-size array so a host is not silently capped
    // at however many aliases the vendored set happened to need.
    std::vector<std::string> languageIds;
    SyntaxLanguageFactory language = nullptr;
    // Highlight query source text, not a path: nothing is read from disk at
    // runtime, and a host may hold its query in a string, a resource, or its
    // own generated translation unit.
    std::string highlightQuery;
    // Query text of the grammar this one derives from, prepended so this
    // grammar's rules override it. Tree-sitter's "; inherits:" directive is a
    // convention its query compiler does not process, so inheritance has to be
    // performed here. Empty when the grammar inherits nothing.
    std::string inheritedHighlightQuery;
    // A grammar to run INSIDE certain nodes of this one, for languages split
    // across two parsers.  Markdown is the case that needs it: upstream parses
    // block structure (headings, lists, code fences) and inline structure
    // (emphasis, code spans, links) with separate grammars, and the block
    // grammar leaves the inline content as opaque `inline` nodes.  Without this
    // the whole inline half of a markdown document is unhighlighted.
    //
    // Deliberately one level and node-type-driven rather than a general
    // injection system: that is what the vendored set needs, and a general one
    // would be untested machinery.
    struct Injection {
        std::string nodeType;  // Nodes of this type carry the injected language.
        SyntaxLanguageFactory language = nullptr;
        std::string highlightQuery;
    };
    std::optional<Injection> injection;
};

// The grammars SSG vendors: C, C++, JavaScript, TypeScript, C#, Lua, and
// Markdown, with their queries compiled into the binary.  Markdown is the block
// grammar only -- upstream splits it in two, and the inline half (emphasis,
// links, code spans) needs a language injection this parser does not implement.
[[nodiscard]] std::vector<TreeSitterGrammar> vendoredTreeSitterGrammars();

// A parser over exactly `grammars`. The set is used as given, NOT merged with
// the vendored set, so a host can replace the defaults rather than only add to
// them; pass `vendoredTreeSitterGrammars()` plus your own to extend.
[[nodiscard]] std::shared_ptr<SyntaxParser> makeTreeSitterParser(
    std::vector<TreeSitterGrammar> grammars);

// The default parser: every vendored grammar.
[[nodiscard]] std::shared_ptr<SyntaxParser> makeTreeSitterParser();

}  // namespace ssg
