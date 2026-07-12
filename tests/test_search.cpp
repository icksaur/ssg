#include "test_helpers.h"

#include <ssg/search.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ssg;

class FixtureWorkspace final : public SearchWorkspaceSource {
public:
    WorkspaceSnapshot snapshot(Revision revision) const override {
        return {
            .revision = revision,
            .files = {
                {.path = "README.md", .text = "project\n"},
                {.path = "include/ssg/search.h",
                 .text = "struct SearchResult {};\n"},
                {.path = "src/search.cpp",
                 .text = "search ranking\nworker\ncancel token\n"},
                {.path = "src/session.cpp", .text = "session state\n"},
            },
            .symbols = {
                {.path = "include/ssg/search.h",
                 .name = "SearchResult",
                 .line = 1,
                 .column = 8},
                {.path = "src/search.cpp",
                 .name = "SearchController",
                 .line = 1,
                 .column = 1},
                {.path = "src/session.cpp",
                 .name = "EditorSession",
                 .line = 1,
                 .column = 1},
            },
        };
    }
};

class FixtureCommands final : public SearchCommandSource {
public:
    std::vector<SearchCommandDescriptor> descriptors() const override {
        return {
            {.id = "file.open", .label = "Open File"},
            {.id = "palette.close", .label = "Close Command Palette"},
            {.id = "workspace.open", .label = "Open Workspace"},
        };
    }

    PaletteExecutionResult execute(std::string_view command_id) override {
        executed.emplace_back(command_id);
        if (command_id == "file.open") {
            return {.accepted = true};
        }
        return {.accepted = false, .message = "command rejected"};
    }

    std::vector<std::string> executed;
};

std::vector<std::string> split(std::string_view value, char separator) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(separator, begin);
        result.emplace_back(value.substr(
            begin, end == std::string_view::npos ? value.size() - begin
                                                  : end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

std::vector<std::string> labels(const std::vector<SearchResult>& results) {
    std::vector<std::string> values;
    values.reserve(results.size());
    for (const auto& result : results) {
        values.push_back(result.label);
    }
    return values;
}

TEST(query_modes_are_unambiguous_and_lines_are_validated) {
    const ParsedSearchQuery file{.mode = SearchMode::file, .text = "file"};
    const ParsedSearchQuery symbol{.mode = SearchMode::symbol, .text = "Widget"};
    const ParsedSearchQuery text{.mode = SearchMode::text, .text = "needle"};
    const ParsedSearchQuery line{
        .mode = SearchMode::line, .text = "42", .line = LineIndex{41}};
    ASSERT_EQ(parse_search_query("file"), file);
    ASSERT_EQ(parse_search_query("@Widget"), symbol);
    ASSERT_EQ(parse_search_query("#needle"), text);
    ASSERT_EQ(parse_search_query(":42"), line);
    ASSERT_EQ(parse_search_query(":0").error, SearchQueryError::invalid_line);
    ASSERT_EQ(parse_search_query(":no").error, SearchQueryError::invalid_line);

    const auto target = goto_line("src/search.cpp", parse_search_query(":42"));
    ASSERT_TRUE(target.has_value());
    ASSERT_EQ(target->line, LineIndex{41});
    ASSERT_FALSE(goto_line({}, parse_search_query(":42")).has_value());
}

TEST(accepted_ranking_goldens_match) {
    FixtureWorkspace workspace;
    const auto snapshot = workspace.snapshot(Revision{7});
    std::ifstream fixture{
        std::filesystem::path{SSG_SEARCH_FIXTURE_DIR} / "ranking.tsv"};
    ASSERT_TRUE(fixture.good());

    std::string row;
    while (std::getline(fixture, row)) {
        if (row.empty() || row.front() == '#') {
            continue;
        }
        const auto fields = split(row, '\t');
        ASSERT_EQ(fields.size(), std::size_t{3});
        if (fields.size() != 3) {
            continue;
        }
        std::string query = fields[1];
        if (fields[0] == "symbol") {
            query.insert(query.begin(), '@');
        } else if (fields[0] == "text") {
            query.insert(query.begin(), '#');
        }
        const auto results =
            rank_workspace(snapshot, parse_search_query(query),
                           SearchCancellationToken{});
        ASSERT_EQ(labels(results), split(fields[2], ','));
    }
}

TEST(cancellation_supersession_and_stale_revision_are_rejected) {
    FixtureWorkspace workspace;
    FixtureCommands commands;
    SearchController controller{workspace, commands};

    const auto first = controller.begin_workspace_search("#search", Revision{8});
    const auto second = controller.begin_workspace_search("#cancel", Revision{8});
    ASSERT_TRUE(first.cancellation.cancelled());

    const auto cancelled = controller.evaluate(first);
    ASSERT_TRUE(cancelled.cancelled);
    ASSERT_EQ(controller.publish(cancelled, Revision{8}),
              SearchPublishResult::cancelled);

    auto superseded = controller.evaluate(second);
    superseded.generation = first.generation;
    ASSERT_EQ(controller.publish(superseded, Revision{8}),
              SearchPublishResult::superseded);

    const auto completed = controller.evaluate(second);
    ASSERT_EQ(controller.publish(completed, Revision{9}),
              SearchPublishResult::stale_revision);
    ASSERT_EQ(controller.publish(completed, Revision{8}),
              SearchPublishResult::accepted);
    ASSERT_EQ(labels(controller.view_state().results),
              std::vector<std::string>{"src/search.cpp:3"});
}

TEST(navigation_history_matches_transition_table) {
    const NavigationTarget a{.path = "a.cpp", .line = LineIndex{1}};
    const NavigationTarget b{.path = "b.cpp", .line = LineIndex{2}};
    const NavigationTarget c{.path = "c.cpp", .line = LineIndex{3}};
    NavigationHistory history{3};

    const auto visit_a = history.visit(a, NavigationOrigin::user);
    const NavigationTransition to_a{.target = a,
                                    .pause_follow_edits = true,
                                    .reveal_primary_caret = true};
    const NavigationTransition to_b{.target = b,
                                    .pause_follow_edits = true,
                                    .reveal_primary_caret = true};
    ASSERT_EQ(visit_a, to_a);
    const auto visit_b = history.visit(b, NavigationOrigin::programmatic);
    ASSERT_FALSE(visit_b.pause_follow_edits);
    ASSERT_EQ(history.back(), to_a);
    ASSERT_EQ(history.forward(), to_b);
    const auto ignored_back = history.back();
    const auto visit_c = history.visit(c, NavigationOrigin::user);
    ASSERT_TRUE(ignored_back.target.has_value());
    ASSERT_TRUE(visit_c.pause_follow_edits);
    ASSERT_FALSE(history.forward().target.has_value());
    ASSERT_EQ(history.back().target, std::optional<NavigationTarget>{a});
    ASSERT_FALSE(history.back().target.has_value());

    NavigationHistory bounded{2};
    const auto ignored_a = bounded.visit(a, NavigationOrigin::user);
    const auto ignored_b = bounded.visit(b, NavigationOrigin::user);
    const auto ignored_c = bounded.visit(c, NavigationOrigin::user);
    ASSERT_TRUE(ignored_a.target.has_value());
    ASSERT_TRUE(ignored_b.target.has_value());
    ASSERT_TRUE(ignored_c.target.has_value());
    ASSERT_EQ(bounded.back().target, std::optional<NavigationTarget>{b});
    ASSERT_FALSE(bounded.back().target.has_value());
    ASSERT_THROWS(NavigationHistory{0}, std::invalid_argument);
}

TEST(palette_uses_injected_catalog_and_dispatch) {
    FixtureWorkspace workspace;
    FixtureCommands commands;
    SearchController controller{workspace, commands};

    controller.open_palette(Revision{11});
    controller.update_palette_query("open f", Revision{12});
    ASSERT_TRUE(controller.view_state().palette_open);
    ASSERT_EQ(labels(controller.view_state().results),
              std::vector<std::string>{"Open File"});
    ASSERT_EQ(controller.execute_palette().accepted, true);
    ASSERT_EQ(commands.executed, std::vector<std::string>{"file.open"});

    controller.close_palette(Revision{13});
    ASSERT_FALSE(controller.execute_palette().accepted);
}

TEST(view_delta_replay_and_command_exports_are_exact) {
    FixtureWorkspace workspace;
    FixtureCommands commands;
    SearchController controller{workspace, commands};
    const auto base = controller.view_state();
    controller.open_palette(Revision{20});
    const auto target = controller.view_state();
    const auto delta = derive_search_delta(base, target);
    const auto replay = replay_search_delta(base, delta);
    ASSERT_TRUE(replay.accepted());
    ASSERT_EQ(*replay.state, target);
    const auto no_change = replay_search_delta(target,
                                               derive_search_delta(target, target));
    ASSERT_TRUE(no_change.accepted());
    ASSERT_EQ(*no_change.state, target);

    auto stale = base;
    stale.revision = Revision{99};
    ASSERT_EQ(replay_search_delta(stale, delta).error,
              SearchReplayError::stale_revision);

    const auto set = search_command_set();
    const std::vector<std::string_view> expected{
        "palette.open",          "palette.close",        "palette.next",
        "palette.previous",      "palette.execute",      "goto.file",
        "goto.line",             "goto.symbol",          "goto.back",
        "goto.forward",          "search.workspace",     "search.results_next",
        "search.results_previous"};
    std::vector<std::string_view> actual;
    for (const auto& descriptor : set.descriptors()) {
        actual.push_back(descriptor.id);
        ASSERT_TRUE(descriptor.user_navigation ==
                        (descriptor.id.starts_with("goto.") ||
                         descriptor.id.starts_with("search.results_")));
    }
    ASSERT_EQ(actual, expected);
}

} // namespace

int main() {
    RUN(query_modes_are_unambiguous_and_lines_are_validated);
    RUN(accepted_ranking_goldens_match);
    RUN(cancellation_supersession_and_stale_revision_are_rejected);
    RUN(navigation_history_matches_transition_table);
    RUN(palette_uses_injected_catalog_and_dispatch);
    RUN(view_delta_replay_and_command_exports_are_exact);
    return failed == 0 ? 0 : 1;
}
