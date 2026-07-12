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
    ASSERT_TRUE(client.open_document("file:///workspace/main.cpp", "cpp",
                                     Revision{1}, std::move(text))
                    .accepted());
}

TEST(command_set_exports_the_normative_read_only_actions) {
    const auto commands = ssg::lsp_feature_command_set().descriptors();
    ASSERT_EQ(commands.size(), std::size_t{9});
    ASSERT_EQ(commands[0].id, std::string_view{"completion.open"});
    ASSERT_EQ(commands[4].id, std::string_view{"completion.dismiss"});
    ASSERT_EQ(commands[5].id, std::string_view{"hover.show"});
    ASSERT_EQ(commands[8].id, std::string_view{"goto.reference"});
}

TEST(request_uses_the_exact_synchronized_snapshot_and_utf16_position) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server, "a\xF0\x9F\x98\x80" "b");
    LspFeatureController features{client};

    const auto request = features.request_completion(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{5});
    ASSERT_TRUE(request.accepted());
    const auto& payload = server.received_payloads().back();
    ASSERT_TRUE(payload.find("\"method\":\"textDocument/completion\"") !=
                std::string::npos);
    ASSERT_TRUE(payload.find("\"line\":0,\"character\":3") !=
                std::string::npos);
}

TEST(completion_results_are_sorted_and_accept_the_selected_edit) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    ASSERT_TRUE(features.request_completion("file:///workspace/main.cpp",
                                            Revision{1}, ByteOffset{1})
                    .accepted());
    server.queue_payload(fixture("completion.json"));
    const auto published = features.poll(Revision{1});
    ASSERT_TRUE(published.accepted());
    ASSERT_EQ(published.publications.front().result,
              LspFeaturePublishResult::accepted);
    ASSERT_EQ(features.view_state().completion.items[0].label,
              std::string{"alpha"});
    ASSERT_EQ(features.view_state().completion.selected_index,
              std::optional<std::size_t>{0});

    features.select_next_completion();
    features.select_previous_completion();
    const auto acceptance = features.accept_completion();
    ASSERT_TRUE(acceptance.accepted);
    ASSERT_EQ(acceptance.text, std::string{"alpha"});
    const auto expected_range = std::optional<LspRange>{
        LspRange{LspPosition{0, 1}, LspPosition{0, 3}}};
    ASSERT_EQ(acceptance.range, expected_range);
    ASSERT_FALSE(features.view_state().completion.visible);
}

TEST(null_completion_result_is_an_accepted_empty_list) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    const auto request = features.request_completion(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{0});
    ASSERT_TRUE(request.accepted());
    server.queue_payload(ssg::test::response(request.request_id, "null"));
    const auto published = features.poll(Revision{1});
    ASSERT_TRUE(published.accepted());
    ASSERT_EQ(published.publications.front().result,
              LspFeaturePublishResult::accepted);
    ASSERT_FALSE(features.view_state().completion.visible);
    ASSERT_FALSE(features.view_state().completion.loading);
    ASSERT_TRUE(features.view_state().completion.items.empty());
}

TEST(stale_and_cancelled_results_do_not_publish_state) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    ASSERT_TRUE(features.request_hover("file:///workspace/main.cpp",
                                       Revision{1}, ByteOffset{0})
                    .accepted());
    server.queue_payload(fixture("hover.json"));
    const auto stale = features.poll(Revision{2});
    ASSERT_EQ(stale.publications.front().result,
              LspFeaturePublishResult::stale_revision);
    ASSERT_FALSE(features.view_state().hover.has_value());

    ASSERT_TRUE(features.request_hover("file:///workspace/main.cpp",
                                       Revision{1}, ByteOffset{0})
                    .accepted());
    features.dismiss_hover();
    auto late = fixture("hover.json");
    const auto id = stale.publications.front().request_id + 1;
    const auto marker = std::string{"\"id\":2"};
    late.replace(late.find(marker), marker.size(),
                 "\"id\":" + std::to_string(id));
    server.queue_payload(std::move(late));
    const auto cancelled = features.poll(Revision{1});
    ASSERT_EQ(cancelled.publications.front().result,
              LspFeaturePublishResult::cancelled);
    ASSERT_FALSE(features.view_state().hover.has_value());
}

TEST(successful_hover_and_invalid_request_are_failure_atomic) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    const auto before = features.view_state();
    ASSERT_FALSE(features.request_hover("file:///workspace/missing.cpp",
                                        Revision{1}, ByteOffset{0})
                     .accepted());
    ASSERT_EQ(features.view_state(), before);

    ASSERT_TRUE(features.request_hover("file:///workspace/main.cpp",
                                       Revision{1}, ByteOffset{0})
                    .accepted());
    server.queue_payload(fixture("hover.json"));
    ASSERT_TRUE(features.poll(Revision{1}).accepted());
    ASSERT_TRUE(features.view_state().hover.has_value());
    ASSERT_EQ(features.view_state().hover->contents,
              std::string{"**symbol** documentation"});
}

TEST(malformed_and_server_error_responses_are_correlated_and_bounded) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    const auto malformed_request = features.request_completion(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{0});
    ASSERT_TRUE(malformed_request.accepted());
    server.queue_payload(ssg::test::response(
        malformed_request.request_id, R"({"items":[{"detail":"no label"}]})"));
    const auto malformed = features.poll(Revision{1});
    ASSERT_EQ(malformed.publications.front().result,
              LspFeaturePublishResult::malformed_response);
    ASSERT_FALSE(features.view_state().completion.loading);
    ASSERT_TRUE(features.view_state().completion.items.empty());

    const auto error_request = features.request_hover(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{0});
    ASSERT_TRUE(error_request.accepted());
    server.queue_payload(
        "{\"jsonrpc\":\"2.0\",\"id\":" +
        std::to_string(error_request.request_id) +
        ",\"error\":{\"code\":-32603,\"message\":\"failed\"}}");
    const auto error = features.poll(Revision{1});
    ASSERT_TRUE(error.accepted());
    ASSERT_EQ(error.publications.front().result,
              LspFeaturePublishResult::server_error);
    ASSERT_EQ(client.state(), ssg::LspLifecycleState::ready);
}

TEST(superseded_completion_response_cannot_replace_the_newer_result) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    const auto first = features.request_completion(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{0});
    const auto second = features.request_completion(
        "file:///workspace/main.cpp", Revision{1}, ByteOffset{1});
    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(second.accepted());
    server.queue_payload(ssg::test::response(first.request_id, "[]"));
    const auto old = features.poll(Revision{1});
    ASSERT_EQ(old.publications.front().result,
              LspFeaturePublishResult::superseded);
    ASSERT_TRUE(features.view_state().completion.loading);

    auto completion = fixture("completion.json");
    const auto marker = std::string{"\"id\":2"};
    completion.replace(completion.find(marker), marker.size(),
                       "\"id\":" + std::to_string(second.request_id));
    server.queue_payload(std::move(completion));
    ASSERT_TRUE(features.poll(Revision{1}).accepted());
    ASSERT_EQ(features.view_state().completion.items[0].label,
              std::string{"alpha"});
}

TEST(definition_and_references_publish_user_navigation_targets) {
    FakeLspServer server;
    LspSyncClient client{server};
    ready(client, server);
    LspFeatureController features{client};

    ASSERT_TRUE(features.request_definition("file:///workspace/main.cpp",
                                            Revision{1}, ByteOffset{0})
                    .accepted());
    server.queue_payload(fixture("definition.json"));
    ASSERT_TRUE(features.poll(Revision{1}).accepted());
    ASSERT_EQ(features.view_state().navigation.targets.size(), std::size_t{1});
    ASSERT_EQ(features.view_state().navigation.targets[0].uri,
              std::string{"file:///workspace/definition.cpp"});
    ASSERT_TRUE(features.view_state().navigation.user_navigation);
    ASSERT_TRUE(features.view_state().navigation.reveal_primary_caret);

    ASSERT_TRUE(features.request_references("file:///workspace/main.cpp",
                                            Revision{1}, ByteOffset{0})
                    .accepted());
    server.queue_payload(fixture("references.json"));
    ASSERT_TRUE(features.poll(Revision{1}).accepted());
    ASSERT_EQ(features.view_state().navigation.targets.size(), std::size_t{2});
    ASSERT_EQ(features.view_state().navigation.selected_index,
              std::optional<std::size_t>{0});
}

TEST(view_delta_round_trip_and_stale_replay) {
    ssg::LspFeatureViewState before;
    before.revision = Revision{7};
    auto after = before;
    after.revision = Revision{8};
    after.status = "ready";
    const auto delta = ssg::derive_lsp_feature_delta(before, after);
    const auto replayed = ssg::replay_lsp_feature_delta(before, delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_EQ(*replayed.state, after);

    auto wrong = before;
    wrong.revision = Revision{6};
    ASSERT_FALSE(ssg::replay_lsp_feature_delta(wrong, delta).accepted());
}

} // namespace

int main() {
    RUN(command_set_exports_the_normative_read_only_actions);
    RUN(request_uses_the_exact_synchronized_snapshot_and_utf16_position);
    RUN(completion_results_are_sorted_and_accept_the_selected_edit);
    RUN(null_completion_result_is_an_accepted_empty_list);
    RUN(stale_and_cancelled_results_do_not_publish_state);
    RUN(successful_hover_and_invalid_request_are_failure_atomic);
    RUN(malformed_and_server_error_responses_are_correlated_and_bounded);
    RUN(superseded_completion_response_cannot_replace_the_newer_result);
    RUN(definition_and_references_publish_user_navigation_targets);
    RUN(view_delta_round_trip_and_stale_replay);
    return failed == 0 ? 0 : 1;
}
