// Proves highlight queries are carried IN the binary rather than read from the
// source tree at runtime.
//
// A separate executable on purpose: TreeSitterParser compiles a query lazily and
// caches it, so any earlier test that highlights a language would make these
// checks vacuous. Only a fresh process is sufficient -- a fresh parser is not.
// (Verified by perturbation: run in-process alongside the golden tests, a
// reverted-to-file-reading implementation passed.)
//
// This does NOT rename or delete anything in the checkout. An earlier version
// hid the vendored .scm files and highlighted, which proved the point but left a
// broken tree if the process died before its restore ran. The same claim holds
// without touching the tree: query text is materialized into each grammar at
// construction, so if it is non-empty and byte-identical to the vendor file it
// cannot have come from a file the parser never opens -- and that no such open
// exists is pinned by the source scan in tests/test_treesitter_syntax.cpp.
#include <ssg/TreeSitterGrammars.h>

#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssg {
// Defined by the generated translation unit.
std::string_view embeddedHighlightQuery(std::string_view key);
}  // namespace ssg

namespace {

using namespace ssg;
namespace fs = std::filesystem;

std::string readFile(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(everyVendoredGrammarCarriesItsQueryTextInMemory) {
    const auto grammars = TreeSitterParserFactory::vendoredGrammars();
    ASSERT_EQ(grammars.size(), std::size_t{7});
    for (const auto& grammar : grammars) {
        ASSERT_FALSE(grammar.languageIds.empty());
        ASSERT_TRUE(grammar.language != nullptr);
        // The query travels with the grammar; nothing is resolved later from a
        // path, so there is no point at which a missing file could degrade it.
        ASSERT_FALSE(grammar.highlightQuery.empty());
        // An injection that names a node type but carries no grammar or query
        // would silently do nothing, which is indistinguishable from having no
        // injection at all until someone opens a markdown file and wonders why
        // half of it is plain.
        if (grammar.injection) {
            ASSERT_FALSE(grammar.injection->nodeType.empty());
            ASSERT_TRUE(grammar.injection->language != nullptr);
            ASSERT_FALSE(grammar.injection->highlightQuery.empty());
        }
    }
    // Markdown is the grammar that needs one: its inline half is a separate
    // parser upstream.
    bool markdownInjects = false;
    for (const auto& grammar : grammars) {
        for (const auto& id : grammar.languageIds) {
            if (id == "markdown" && grammar.injection) markdownInjects = true;
        }
    }
    ASSERT_TRUE(markdownInjects);
}

// Reference-implementation oracle: the test reads the vendor file itself and
// compares, so a generator that truncates, mis-escapes, or drops an entry fails
// here rather than producing subtly wrong highlighting.
TEST(embeddedQueryTextMatchesTheVendorFilesByteForByte) {
    const fs::path vendorRoot = fs::path{SSG_TREESITTER_VENDOR_DIR};
    const std::vector<std::pair<std::string, std::string>> keyToFile{
        {"c", "tree-sitter-c/queries/highlights.scm"},
        {"cpp", "tree-sitter-cpp/queries/highlights.scm"},
        {"javascript", "tree-sitter-javascript/queries/highlights.scm"},
        {"typescript", "tree-sitter-typescript/queries/highlights.scm"},
        {"csharp", "tree-sitter-c-sharp/queries/highlights.scm"},
        {"lua", "tree-sitter-lua/queries/highlights.scm"},
        {"markdown", "tree-sitter-markdown/queries/highlights.scm"},
        {"markdown_inline",
         "tree-sitter-markdown-inline/queries/highlights.scm"},
    };
    for (const auto& [key, relative] : keyToFile) {
        const auto expected = readFile(vendorRoot / relative);
        ASSERT_FALSE(expected.empty());
        ASSERT_EQ(std::string{embeddedHighlightQuery(key)}, expected);
    }
    // An unknown key must be observably absent rather than fabricated, or the
    // check above could pass against a table that returns something for
    // everything.
    ASSERT_TRUE(embeddedHighlightQuery("no-such-grammar").empty());
}

// The inherited-query prepend is a real artifact accommodation: tree-sitter's
// "; inherits:" directive is a convention its query compiler ignores, so C++ and
// TypeScript would silently lose their base grammar's rules without it.
TEST(derivedGrammarsCarryTheirInheritedQueryText) {
    for (const auto& grammar : TreeSitterParserFactory::vendoredGrammars()) {
        const auto& id = grammar.languageIds.front();
        const bool derived = id == "cpp" || id == "typescript";
        ASSERT_EQ(!grammar.inheritedHighlightQuery.empty(), derived);
    }
}

}  // namespace

int main() {
    RUN(everyVendoredGrammarCarriesItsQueryTextInMemory);
    RUN(embeddedQueryTextMatchesTheVendorFilesByteForByte);
    RUN(derivedGrammarsCarryTheirInheritedQueryText);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
