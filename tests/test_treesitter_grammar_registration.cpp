// The public grammar-registration seam (doc/spec-grammar-pipeline.md Phase C).
//
// This translation unit deliberately includes NO tree-sitter header. That is
// half the point of the seam: a host registers a grammar through
// <ssg/TreeSitterGrammars.h> alone, and the opaque SyntaxLanguageHandle is what
// keeps tree-sitter's types out of SSG's public surface. If this file ever
// needs <tree_sitter/api.h> to compile, the seam has leaked.
#include <ssg/TreeSitterGrammars.h>

#include <ssg/SyntaxModel.h>

#include "test_helpers.h"

#include <cstdint>
#include <string>
#include <vector>

// The C grammar's entry point, declared by hand rather than by including
// tree-sitter's headers, exactly as a host linking its own grammar would.
// TSLanguage is left INCOMPLETE: naming the real return type keeps this
// declaration compatible with the definition -- declaring it as returning
// `const void*` would be a type mismatch across translation units -- while
// still requiring no tree-sitter header, since an incomplete type is all a
// pointer return needs.
struct TSLanguage;
extern "C" const TSLanguage* tree_sitter_c();

namespace {

using namespace ssg;

bool anyScope(const std::vector<SyntaxSpan>& spans, SyntaxScope scope) {
    for (const auto& span : spans) {
        if (span.scope == scope) return true;
    }
    return false;
}

std::vector<SyntaxSpan> spansFor(SyntaxParser& parser,
                                 const std::string& language,
                                 const std::string& text,
                                 std::uint64_t revision) {
    SyntaxModel model{std::shared_ptr<SyntaxParser>{&parser, [](SyntaxParser*) {}}};
    const auto request = model.request(Revision{revision}, LanguageId{language}, text);
    if (!request.accepted()) return {};
    return model.run(*request.request).spans;
}

// A grammar registered under a name SSG does not vendor, reusing the C language
// so the test needs no grammar of its own.  Proves registration is genuinely
// open: the id is what SSG looks up, and it is not in the vendored set.
TreeSitterGrammar customGrammar() {
    TreeSitterGrammar grammar;
    grammar.languageIds = {"ssg-test-lang"};
    grammar.language = []() -> SyntaxLanguageHandle { return tree_sitter_c(); };
    // "return" is an anonymous token in the C grammar; "int" is not (it parses
    // as primitive_type), and querying a nonexistent node type fails to compile.
    grammar.highlightQuery = "\"return\" @keyword\n(identifier) @variable\n";
    return grammar;
}

TEST(aCustomGrammarIsHighlightedUnderItsOwnLanguageId) {
    auto parser = makeTreeSitterParser({customGrammar()});
    ASSERT_TRUE(parser != nullptr);
    if (!parser) return;
    ASSERT_TRUE(parser->hasGrammar(LanguageId{"ssg-test-lang"}));

    const auto spans =
        spansFor(*parser, "ssg-test-lang", "int main() { return 0; }\n", 1);
    ASSERT_FALSE(spans.empty());
    ASSERT_TRUE(anyScope(spans, SyntaxScope::Keyword));
}

// The registered set REPLACES the vendored set rather than extending it.  A
// merging implementation would still highlight "c" here, so this is what makes
// the substitution claim falsifiable.
TEST(aCustomGrammarSetReplacesRatherThanExtendsTheVendoredOne) {
    auto parser = makeTreeSitterParser({customGrammar()});
    ASSERT_TRUE(parser != nullptr);
    if (!parser) return;

    ASSERT_TRUE(parser->hasGrammar(LanguageId{"ssg-test-lang"}));
    ASSERT_FALSE(parser->hasGrammar(LanguageId{"c"}));
    ASSERT_FALSE(parser->hasGrammar(LanguageId{"cpp"}));
    ASSERT_FALSE(parser->hasGrammar(LanguageId{"lua"}));
}

TEST(theDefaultParserStillCarriesEveryVendoredGrammar) {
    auto parser = makeTreeSitterParser();
    ASSERT_TRUE(parser != nullptr);
    if (!parser) return;
    for (const auto* id : {"c", "cpp", "c++", "javascript", "js", "typescript",
                           "ts", "csharp", "c#", "lua"}) {
        ASSERT_TRUE(parser->hasGrammar(LanguageId{id}));
    }
    ASSERT_FALSE(parser->hasGrammar(LanguageId{"ssg-test-lang"}));
}

// Two parsers may now hold DIFFERENT query text for the same language id, so a
// cache shared across parsers and keyed by id would serve one parser's query to
// the other.  Registering "c" with a query that colors nothing must not disturb
// the default parser's real C highlighting, in either order.
TEST(parsersDoNotShareCompiledQueriesForTheSameLanguageId) {
    TreeSitterGrammar shadowed;
    shadowed.languageIds = {"c"};
    shadowed.language = []() -> SyntaxLanguageHandle { return tree_sitter_c(); };
    // Valid query, but captures nothing that maps to a scope.
    shadowed.highlightQuery = "(translation_unit) @none\n";

    auto shadowedParser = makeTreeSitterParser({shadowed});
    auto defaultParser = makeTreeSitterParser();
    ASSERT_TRUE(shadowedParser && defaultParser);
    if (!shadowedParser || !defaultParser) return;

    const std::string text = "int main() { return 0; }\n";
    const auto shadowedSpans = spansFor(*shadowedParser, "c", text, 1);
    const auto defaultSpans = spansFor(*defaultParser, "c", text, 2);

    ASSERT_FALSE(anyScope(shadowedSpans, SyntaxScope::Keyword));
    ASSERT_TRUE(anyScope(defaultSpans, SyntaxScope::Keyword));
}

// A grammar with no language factory is ignored rather than crashing: the
// struct is public, so a host can leave it default-constructed.
TEST(aGrammarWithNoLanguageFactoryIsIgnored) {
    TreeSitterGrammar incomplete;
    incomplete.languageIds = {"broken"};
    incomplete.highlightQuery = "\"return\" @keyword\n";

    auto parser = makeTreeSitterParser({incomplete});
    ASSERT_TRUE(parser != nullptr);
    if (!parser) return;
    ASSERT_FALSE(parser->hasGrammar(LanguageId{"broken"}));
}

}  // namespace

int main() {
    RUN(aCustomGrammarIsHighlightedUnderItsOwnLanguageId);
    RUN(aCustomGrammarSetReplacesRatherThanExtendsTheVendoredOne);
    RUN(theDefaultParserStillCarriesEveryVendoredGrammar);
    RUN(parsersDoNotShareCompiledQueriesForTheSameLanguageId);
    RUN(aGrammarWithNoLanguageFactoryIsIgnored);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
