#include "ssg/EditorRuntime.h"
#include "ssg/SyntaxModel.h"
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
// uses the parser handed to it via EditorRuntimeConfig, not a hard-constructed one.
class RecordingParser final : public SyntaxParser {
public:
    std::shared_ptr<std::size_t> parseCalls = std::make_shared<std::size_t>(0);

    bool hasGrammar(const LanguageId&) const override { return true; }

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
};

std::filesystem::path uniqueRoot() {
    static std::atomic<int> counter{0};
    auto root = std::filesystem::temp_directory_path() /
                ("ssg-syntax-injection-" + std::to_string(++counter) + "-" +
                 std::to_string(std::filesystem::hash_value(
                     std::filesystem::current_path())));
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

EditorRuntimeConfig configFor(const std::filesystem::path& root) {
    EditorRuntimeConfig config;
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

    auto created = EditorRuntime::create(std::move(config));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ClientId{1}, InvocationOrigin::InProcess},
                            ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"main.cpp"}})
                    .accepted());

    auto snapshot = runtime.snapshot(ClientId{1}, ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot.has_value()) return;
    ASSERT_TRUE(*calls > 0);
    ASSERT_TRUE(hasScope(snapshot->sections().syntax, SyntaxScope::Keyword));
}

// Without an injected parser the runtime falls back to plain-text spans.
TEST(nullParserYieldsPlainText) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "main.cpp"} << "int main() {}";

    auto created = EditorRuntime::create(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime
                    .attach({ClientId{1}, InvocationOrigin::InProcess},
                            ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"main.cpp"}})
                    .accepted());

    auto snapshot = runtime.snapshot(ClientId{1}, ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot.has_value()) return;
    ASSERT_FALSE(hasScope(snapshot->sections().syntax, SyntaxScope::Keyword));
}

}  // namespace

int main() {
    RUN(injectedParserDrivesHighlighting);
    RUN(nullParserYieldsPlainText);
    return failed == 0 ? 0 : 1;
}
