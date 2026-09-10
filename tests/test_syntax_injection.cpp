#include <ssg/Editor.h>
#include "grid_test_frame.h"
#include <ssg/SyntaxModel.h>
#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <atomic>
#include <memory>
#include <string>

namespace {

using namespace ssg;

// A minimal opaque parse payload for the recording parser.
class RecordingParse final : public OpaqueSyntaxParse {
public:
    explicit RecordingParse(std::string text) : text_(std::move(text)) {}

private:
    std::string text_;
};

// A SyntaxParser test double that tags the entire document as Keyword and counts
// how many times the runtime drove it. It proves the injection seam: the runtime
// uses the parser handed to it via EditorConfig, not a hard-constructed one.
class RecordingParser final : public SyntaxParser {
public:
    explicit RecordingParser(bool grammarAvailable = true)
        : grammarAvailable_(grammarAvailable) {}

    std::shared_ptr<std::size_t> parseCalls = std::make_shared<std::size_t>(0);

    bool hasGrammar(const LanguageId&) const override { return grammarAvailable_; }

    SyntaxParseOutput parse(const SyntaxParseRequest& request) override {
        ++*parseCalls;
        SyntaxParseOutput output;
        output.revision = request.revision();
        if (request.cancelled()) {
            output.status = SyntaxParseStatus::Cancelled;
            return output;
        }
        output.status = SyntaxParseStatus::Parsed;
        output.parse = std::make_shared<RecordingParse>(request.text());
        if (!request.text().empty()) {
            output.spans.push_back(
                {ByteOffset{0}, ByteOffset{request.text().size()},
                 SyntaxScope::Keyword});
        }
        return output;
    }

private:
    bool grammarAvailable_ = true;
};

std::filesystem::path uniqueRoot() {
    static std::atomic<int> counter{0};
    auto root =
        testRuntimePath("runtime_syntax_injection_" + std::to_string(++counter));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    return root;
}

bool hasScope(const SyntaxViewState& syntax, SyntaxScope scope) {
    for (const auto& span : syntax.spans()) {
        if (span.scope == scope) return true;
    }
    return false;
}

EditorConfig configFor(const std::filesystem::path& root) {
    EditorConfig config;
    config.cwd = root / "workspace";
    config.scratchRoot = root / "scratch";
    config.recoveryRoot = root / "recovery";
    return config;
}

// The runtime drives the injected parser and its scopes reach the snapshot.
TEST(injectedParserDrivesHighlighting) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "main.cpp"} << "int main() {}";

    auto parser = std::make_shared<RecordingParser>();
    auto calls = parser->parseCalls;
    auto config = configFor(root);
    config.syntaxParser = parser;

    auto created = createEditor(std::move(config));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"main.cpp"}})
                    .accepted());

    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot.has_value()) return;
    ASSERT_TRUE(*calls > 0);
    ASSERT_TRUE(hasScope(snapshot->syntax, SyntaxScope::Keyword));
}

// Without an injected parser the runtime falls back to plain-text spans.
// The ONLY way to disable highlighting, now that tree-sitter is compiled
// unconditionally: construct the runtime
// with no parser and every span stays plain. Before Phase B a build could also
// opt out at compile time, so this test was one of two proofs; it is now the
// only one.
TEST(nullParserYieldsPlainText) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "main.cpp"} << "int main() {}";

    auto created = createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"main.cpp"}})
                    .accepted());

    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot.has_value()) return;
    ASSERT_FALSE(hasScope(snapshot->syntax, SyntaxScope::Keyword));
}

// With deferred enrichment enabled, a grammar-backed small file is still parsed
// on open so the first frame already carries syntax colors.
TEST(deferredEnrichmentStillColorsSmallGrammarBackedFirstFrame) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "main.cpp"} << "int main() {}";

    auto parser = std::make_shared<RecordingParser>();
    auto calls = parser->parseCalls;
    auto config = configFor(root);
    config.deferEnrichment = true;
    config.syntaxParser = parser;

    auto created = createEditor(std::move(config));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"main.cpp"}})
                    .accepted());

    auto first = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(first.has_value());
    if (!first.has_value()) return;
    ASSERT_TRUE(*calls > 0);
    ASSERT_TRUE(hasScope(first->syntax, SyntaxScope::Keyword));
}

// Large files stay deferred under deferEnrichment even with an available grammar:
// first frame is plain text, then primeDeferred applies syntax.
TEST(deferredEnrichmentDefersLargeGrammarBackedFileUntilPrimeDeferred) {
    auto root = uniqueRoot();
    std::string text(3 * 1024 * 1024, 'a');
    std::ofstream{root / "workspace" / "big.cpp"} << text;

    auto parser = std::make_shared<RecordingParser>();
    auto calls = parser->parseCalls;
    auto config = configFor(root);
    config.deferEnrichment = true;
    config.syntaxParser = parser;

    auto created = createEditor(std::move(config));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"big.cpp"}})
                    .accepted());

    auto first = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(first.has_value());
    if (!first.has_value()) return;
    ASSERT_EQ(*calls, std::size_t{0});
    ASSERT_FALSE(hasScope(first->syntax, SyntaxScope::Keyword));

    runtime.primeDeferred();
    auto after = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(after.has_value());
    if (!after.has_value()) return;
    ASSERT_TRUE(*calls > 0);
    ASSERT_TRUE(hasScope(after->syntax, SyntaxScope::Keyword));
}

// Syntax state is document-owned: switching back to a deferred large file must
// never show keyword spans parsed for another tab.
TEST(deferredLargeTabNeverBorrowsAnotherTabsSyntaxState) {
    auto root = uniqueRoot();
    std::string large = "alpha = 1;\n";
    large.append(3 * 1024 * 1024, 'x');
    std::ofstream{root / "workspace" / "fileA.cpp"} << large;
    std::ofstream{root / "workspace" / "fileB.cpp"} << "return b;\n";

    auto parser = std::make_shared<RecordingParser>();
    auto config = configFor(root);
    config.deferEnrichment = true;
    config.syntaxParser = parser;

    auto created = createEditor(std::move(config));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;

    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"fileA.cpp"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"fileB.cpp"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"fileA.cpp"}})
                    .accepted());

    auto firstA = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(firstA.has_value());
    if (!firstA.has_value()) return;
    ASSERT_FALSE(hasScope(firstA->syntax, SyntaxScope::Keyword));

    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"fileB.cpp"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"text.insert",
                               TextInputArguments{"z"}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch({"file.open",
                               std::string{"fileA.cpp"}})
                    .accepted());

    auto secondA = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(secondA.has_value());
    if (!secondA.has_value()) return;
    ASSERT_FALSE(hasScope(secondA->syntax, SyntaxScope::Keyword));
}

}  // namespace

SSG_TEST_SUITE(test_syntax_injection) {
    RUN(injectedParserDrivesHighlighting);
    RUN(nullParserYieldsPlainText);
    RUN(deferredEnrichmentStillColorsSmallGrammarBackedFirstFrame);
    RUN(deferredEnrichmentDefersLargeGrammarBackedFileUntilPrimeDeferred);
    RUN(deferredLargeTabNeverBorrowsAnotherTabsSyntaxState);
    return failed == 0 ? 0 : 1;
}
