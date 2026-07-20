#include "fake_lsp_server.h"
#include "test_helpers.h"

#include <ssg/lsp_features.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using ssg::ByteOffset;
using ssg::LspFeatureController;
using ssg::LspFeaturePublishResult;
using ssg::LspPosition;
using ssg::LspRange;
using ssg::LspSyncClient;
using ssg::Revision;
using ssg::test::FakeLspServer;

std::string fixture(std::string_view name) {
    std::ifstream input(std::string{SSG_TEST_SOURCE_DIR} +
                        "/tests/fixtures/lsp/features/" + std::string{name});
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

void ready(LspSyncClient& client, FakeLspServer& server,
           std::string text = "symbol") {
    ASSERT_TRUE(client.initialize("file:///workspace").accepted());
    server.queue_payload(ssg::test::response(1, "{\"capabilities\":{}}"));
    ASSERT_TRUE(client.poll().accepted());
    ASSERT_TRUE(client.openDocument("file:///workspace/main.cpp", "cpp",
                                     Revision{1}, std::move(text))
                    .accepted());
}

TEST(commandSetExportsTheNormativeReadOnlyActions) {
    const auto commands = ssg::lspFeatureCommandSet().descriptors();
    ASSERT_EQ(commands.size(), std::size_t{9});
    ASSERT_EQ(commands[0].id, std::string_view{"completion.open"});
    ASSERT_EQ(commands[4].id, std::string_view{"completion.dismiss"});
    ASSERT_EQ(commands[5].id, std::string_view{"hover.show"});
    ASSERT_EQ(commands[8].id, std::string_view{"goto.reference"});
}

TEST(requestUsesTheExactSynchronizedSnapshotAndUtf16Position) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server, "a\xF0\x9F\x98\x80" "b");
    LspFeatureController features{client};

    const auto request = features.requestCompletion(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{5});
    ASSERT_TRUE(request.accepted());
    const auto& payload = server.received_payloads().back();
    ASSERT_TRUE(payload.find("\"method\":\"textDocument/completion\"") !=
                std::string::npos);
    ASSERT_TRUE(payload.find("\"line\":0,\"character\":3") !=
                std::string::npos);
}

TEST(completionResultsAreSortedAndAcceptTheSelectedEdit) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    ASSERT_TRUE(features.requestCompletion("file:///workspace/main.cpp",
                                            Revision{1}, ByteOffset{1})
                    .accepted());
    server.queue_payload(fixture("completion.json"));
    const auto published = features.poll(Revision{1});
    ASSERT_TRUE(published.accepted());
    ASSERT_EQ(published.publications.front().result,
              LspFeaturePublishResult::Accepted);
    ASSERT_EQ(features.viewState().completion.items[0].label,
              std::string{"alpha"});
    ASSERT_EQ(features.viewState().completion.selectedIndex,
              std::optional<std::size_t>{0});

    features.selectNextCompletion();
    features.selectPreviousCompletion();
    const auto acceptance = features.acceptCompletion();
    ASSERT_TRUE(acceptance.accepted);
    ASSERT_EQ(acceptance.text, std::string{"alpha"});
    const auto expectedRange = std::optional<LspRange>{
        LspRange{LspPosition{0, 1}, LspPosition{0, 3}}};
    ASSERT_EQ(acceptance.range, expectedRange);
    ASSERT_FALSE(features.viewState().completion.visible);
}

TEST(nullCompletionResultIsAnAcceptedEmptyList) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    const auto request = features.requestCompletion(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{0});
    ASSERT_TRUE(request.accepted());
    server.queue_payload(ssg::test::response(request.requestId, "null"));
    const auto published = features.poll(Revision{1});
    ASSERT_TRUE(published.accepted());
    ASSERT_EQ(published.publications.front().result,
              LspFeaturePublishResult::Accepted);
    ASSERT_FALSE(features.viewState().completion.visible);
    ASSERT_FALSE(features.viewState().completion.loading);
    ASSERT_TRUE(features.viewState().completion.items.empty());
}

TEST(staleAndCancelledResultsDoNotPublishState) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    ASSERT_TRUE(features.requestHover("file:///workspace/main.cpp",
                                       Revision{1}, ByteOffset{0})
                    .accepted());
    server.queue_payload(fixture("hover.json"));
    const auto stale = features.poll(Revision{2});
    ASSERT_EQ(stale.publications.front().result,
              LspFeaturePublishResult::StaleRevision);
    ASSERT_FALSE(features.viewState().hover.has_value());

    ASSERT_TRUE(features.requestHover("file:///workspace/main.cpp",
                                       Revision{1}, ByteOffset{0})
                    .accepted());
    features.dismissHover();
    auto late = fixture("hover.json");
    const auto id = stale.publications.front().requestId + 1;
    const auto marker = std::string{"\"id\":2"};
    late.replace(late.find(marker), marker.size(),
                 "\"id\":" + std::to_string(id));
    server.queue_payload(std::move(late));
    const auto cancelled = features.poll(Revision{1});
    ASSERT_EQ(cancelled.publications.front().result,
              LspFeaturePublishResult::Cancelled);
    ASSERT_FALSE(features.viewState().hover.has_value());
}

TEST(successfulHoverAndInvalidRequestAreFailureAtomic) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    const auto before = features.viewState();
    ASSERT_FALSE(features.requestHover("file:///workspace/missing.cpp",
                                        Revision{1}, ByteOffset{0})
                     .accepted());
    ASSERT_EQ(features.viewState(), before);

    ASSERT_TRUE(features.requestHover("file:///workspace/main.cpp",
                                       Revision{1}, ByteOffset{0})
                    .accepted());
    server.queue_payload(fixture("hover.json"));
    ASSERT_TRUE(features.poll(Revision{1}).accepted());
    ASSERT_TRUE(features.viewState().hover.has_value());
    ASSERT_EQ(features.viewState().hover->contents,
              std::string{"**symbol** documentation"});
}

TEST(malformedAndServerErrorResponsesAreCorrelatedAndBounded) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    const auto malformedRequest = features.requestCompletion(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{0});
    ASSERT_TRUE(malformedRequest.accepted());
    server.queue_payload(ssg::test::response(
        malformedRequest.requestId, R"({"items":[{"detail":"no label"}]})"));
    const auto malformed = features.poll(Revision{1});
    ASSERT_EQ(malformed.publications.front().result,
              LspFeaturePublishResult::MalformedResponse);
    ASSERT_FALSE(features.viewState().completion.loading);
    ASSERT_TRUE(features.viewState().completion.items.empty());

    const auto errorRequest = features.requestHover(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{0});
    ASSERT_TRUE(errorRequest.accepted());
    server.queue_payload(
        "{\"jsonrpc\":\"2.0\",\"id\":" +
        std::to_string(errorRequest.requestId) +
        ",\"error\":{\"code\":-32603,\"message\":\"failed\"}}");
    const auto error = features.poll(Revision{1});
    ASSERT_TRUE(error.accepted());
    ASSERT_EQ(error.publications.front().result,
              LspFeaturePublishResult::ServerError);
    ASSERT_EQ(client.state(), ssg::LspLifecycleState::Ready);
}

TEST(supersededCompletionResponseCannotReplaceTheNewerResult) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    const auto first = features.requestCompletion(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{0});
    const auto second = features.requestCompletion(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{1});
    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    server.queue_payload(ssg::test::response(first.requestId, "[]"));
    const auto old = features.poll(Revision{1});
    ASSERT_EQ(old.publications.front().result,
              LspFeaturePublishResult::Superseded);
    ASSERT_TRUE(features.viewState().completion.loading);

    auto completion = fixture("completion.json");
    const auto marker = std::string{"\"id\":2"};
    completion.replace(completion.find(marker), marker.size(),
                       "\"id\":" + std::to_string(second.requestId));
    server.queue_payload(std::move(completion));
    ASSERT_TRUE(features.poll(Revision{1}).accepted());
    ASSERT_EQ(features.viewState().completion.items[0].label,
              std::string{"alpha"});
}

TEST(definitionAndReferencesPublishUserNavigationTargets) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    ASSERT_TRUE(features.requestDefinition("file:///workspace/main.cpp",
                                            Revision{1}, ByteOffset{0})
                    .accepted());
    server.queue_payload(fixture("definition.json"));
    ASSERT_TRUE(features.poll(Revision{1}).accepted());
    ASSERT_EQ(features.viewState().navigation.targets.size(), std::size_t{1});
    ASSERT_EQ(features.viewState().navigation.targets[0].uri,
              std::string{"file:///workspace/definition.cpp"});
    ASSERT_TRUE(features.viewState().navigation.userNavigation);
    ASSERT_TRUE(features.viewState().navigation.revealPrimaryCaret);

    ASSERT_TRUE(features.requestReferences("file:///workspace/main.cpp",
                                            Revision{1}, ByteOffset{0})
                    .accepted());
    server.queue_payload(fixture("references.json"));
    ASSERT_TRUE(features.poll(Revision{1}).accepted());
    ASSERT_EQ(features.viewState().navigation.targets.size(), std::size_t{2});
    ASSERT_EQ(features.viewState().navigation.selectedIndex,
              std::optional<std::size_t>{0});
}

TEST(viewDeltaRoundTripAndStaleReplay) {
    ssg::LspFeatureViewState before;
    before.revision = Revision{7};
    auto after = before;
    after.revision = Revision{8};
    after.status = "ready";
    const auto delta = ssg::deriveLspFeatureDelta(before, after);
    const auto replayed = ssg::replayLspFeatureDelta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.state, after);

    auto wrong = before;
    wrong.revision = Revision{6};
    ASSERT_FALSE(ssg::replayLspFeatureDelta(wrong, delta).accepted());
}

} // namespace

int main() {
    RUN(commandSetExportsTheNormativeReadOnlyActions);
    RUN(requestUsesTheExactSynchronizedSnapshotAndUtf16Position);
    RUN(completionResultsAreSortedAndAcceptTheSelectedEdit);
    RUN(nullCompletionResultIsAnAcceptedEmptyList);
    RUN(staleAndCancelledResultsDoNotPublishState);
    RUN(successfulHoverAndInvalidRequestAreFailureAtomic);
    RUN(malformedAndServerErrorResponsesAreCorrelatedAndBounded);
    RUN(supersededCompletionResponseCannotReplaceTheNewerResult);
    RUN(definitionAndReferencesPublishUserNavigationTargets);
    RUN(viewDeltaRoundTripAndStaleReplay);
    return failed == 0 ? 0 : 1;
}
