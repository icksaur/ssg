#include "grid_test_frame.h"
#include <ssg/SyntaxModel.h>
#include "test_helpers.h"
#include "editor_test_support.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>

namespace {

using namespace ssg;

class RecordingParse final : public OpaqueSyntaxParse {};

class RecordingParser final : public SyntaxParser {
public:
    explicit RecordingParser(bool blockFirst = false)
        : blockFirst_{blockFirst} {}

    bool hasGrammar(const LanguageId&) const override { return true; }

    SyntaxParseOutput parse(const SyntaxParseRequest& request) override {
        const auto call = calls_.fetch_add(1);
        entered_.release();
        if (blockFirst_ && call == 0) release_.acquire();

        SyntaxParseOutput output;
        output.revision = request.revision();
        if (request.cancelled()) {
            output.status = SyntaxParseStatus::Cancelled;
        } else {
            output.status = SyntaxParseStatus::Parsed;
            output.parse = std::make_shared<RecordingParse>();
            if (!request.text().empty()) {
                output.spans.push_back(
                    {ByteOffset{0}, ByteOffset{request.text().size()},
                     SyntaxScope::Keyword});
            }
        }
        completed_.release();
        return output;
    }

    void waitUntilEntered() { entered_.acquire(); }
    void waitUntilCompleted() { completed_.acquire(); }
    void releaseFirst() { release_.release(); }
    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_.load();
    }

private:
    bool blockFirst_ = false;
    std::atomic<std::size_t> calls_{0};
    std::counting_semaphore<> entered_{0};
    std::counting_semaphore<> completed_{0};
    std::binary_semaphore release_{0};
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
    config.recoveryRoot = root / "recovery";
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    return config;
}

bool pumpUntilAccepted(Editor& editor) {
    for (std::size_t attempt = 0; attempt < 10000; ++attempt) {
        if (editor.pumpSyntax()) return true;
        std::this_thread::yield();
    }
    return false;
}

TEST(editProjectsBeforeBlockedSyntaxAndHighlightsAfterPump) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "main.cpp"} << "int main() {}";

    auto parser = std::make_shared<RecordingParser>(true);
    auto config = configFor(root);
    config.syntaxParser = parser;
    auto created = createEditor(std::move(config));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(
        ssg::test::openFile(runtime, std::string{"main.cpp"}).accepted());

    parser->waitUntilEntered();
    ASSERT_TRUE(ssg::test::typeText(runtime, "x").accepted());
    auto pending = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(pending.has_value());
    if (!pending) return;
    ASSERT_EQ(pending->documentText.front(), 'x');
    ASSERT_FALSE(hasScope(*pending->syntax, SyntaxScope::Keyword));

    parser->releaseFirst();
    parser->waitUntilCompleted();
    parser->waitUntilCompleted();
    ASSERT_TRUE(pumpUntilAccepted(runtime));
    auto enriched = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(enriched.has_value());
    if (!enriched) return;
    ASSERT_EQ(enriched->syntax->revision(), enriched->documentRevision);
    ASSERT_TRUE(hasScope(*enriched->syntax, SyntaxScope::Keyword));
}

TEST(nullParserYieldsPlainTextSynchronously) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "main.cpp"} << "int main() {}";

    auto created = createEditor(configFor(root));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(
        ssg::test::openFile(runtime, std::string{"main.cpp"}).accepted());

    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->syntax->revision(), snapshot->documentRevision);
    ASSERT_FALSE(hasScope(*snapshot->syntax, SyntaxScope::Keyword));
}

TEST(supersededCompletionCannotReplaceLatestRevision) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "main.cpp"} << "int main() {}";

    auto parser = std::make_shared<RecordingParser>(true);
    auto config = configFor(root);
    config.syntaxParser = parser;
    auto created = createEditor(std::move(config));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(
        ssg::test::openFile(runtime, std::string{"main.cpp"}).accepted());
    parser->waitUntilEntered();

    ASSERT_TRUE(ssg::test::typeText(runtime, "a").accepted());
    ASSERT_TRUE(ssg::test::typeText(runtime, "b").accepted());
    const auto latestRevision = runtime.activeDocument()->revision();
    parser->releaseFirst();
    parser->waitUntilCompleted();
    parser->waitUntilCompleted();

    ASSERT_TRUE(pumpUntilAccepted(runtime));
    auto snapshot = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->syntax->revision(), latestRevision);
    ASSERT_EQ(snapshot->syntax->textBytes(), snapshot->documentText.size());
    ASSERT_TRUE(hasScope(*snapshot->syntax, SyntaxScope::Keyword));
}

TEST(projectedSyntaxLifetimeSurvivesLaterAcceptance) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "main.cpp"} << "int main() {}";

    auto parser = std::make_shared<RecordingParser>();
    auto config = configFor(root);
    config.syntaxParser = parser;
    auto created = createEditor(std::move(config));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(
        ssg::test::openFile(runtime, std::string{"main.cpp"}).accepted());
    parser->waitUntilCompleted();
    ASSERT_TRUE(pumpUntilAccepted(runtime));
    auto before = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(before.has_value());
    if (!before) return;
    auto retained = before->syntax;

    ASSERT_TRUE(ssg::test::typeText(runtime, "z").accepted());
    parser->waitUntilCompleted();
    ASSERT_TRUE(pumpUntilAccepted(runtime));
    auto after = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(after.has_value());
    if (!after) return;
    ASSERT_TRUE(retained != after->syntax);
    ASSERT_TRUE(hasScope(*retained, SyntaxScope::Keyword));
    ASSERT_TRUE(hasScope(*after->syntax, SyntaxScope::Keyword));
}

TEST(pathLanguageChangeQueuesReplacementSyntaxAtSameRevision) {
    auto root = uniqueRoot();
    std::ofstream{root / "workspace" / "main.cpp"} << "int main() {}";

    auto parser = std::make_shared<RecordingParser>();
    auto config = configFor(root);
    config.syntaxParser = parser;
    auto created = createEditor(std::move(config));
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(
        ssg::test::openFile(runtime, std::string{"main.cpp"}).accepted());
    parser->waitUntilCompleted();
    ASSERT_TRUE(pumpUntilAccepted(runtime));
    const auto revision = runtime.activeDocument()->revision();
    ASSERT_EQ(runtime.activeSyntaxView()->language(), LanguageId{"cpp"});

    ASSERT_TRUE(applyFilePathCompletion(
                    runtime, PromptCompletion::FileRename, "renamed.md")
                    .accepted);
    ASSERT_EQ(runtime.activeDocument()->revision(), revision);
    parser->waitUntilCompleted();
    ASSERT_TRUE(pumpUntilAccepted(runtime));
    ASSERT_EQ(runtime.activeSyntaxView()->language(), LanguageId{"markdown"});
    ASSERT_EQ(runtime.activeSyntaxView()->revision(), revision);
}

}  // namespace

SSG_TEST_SUITE(test_syntax_injection) {
    RUN(editProjectsBeforeBlockedSyntaxAndHighlightsAfterPump);
    RUN(nullParserYieldsPlainTextSynchronously);
    RUN(supersededCompletionCannotReplaceLatestRevision);
    RUN(projectedSyntaxLifetimeSurvivesLaterAcceptance);
    RUN(pathLanguageChangeQueuesReplacementSyntaxAtSameRevision);
    return failed == 0 ? 0 : 1;
}
