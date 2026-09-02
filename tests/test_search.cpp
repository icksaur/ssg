#include "test_helpers.h"

#include <ssg/Search.h>

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
                {.path = "include/core/ssg/search.h",
                 .text = "struct SearchResult {};\n"},
                {.path = "src/search.cpp",
                 .text = "search ranking\nworker\ncancel token\n"},
                {.path = "src/session.cpp", .text = "session state\n"},
            },
            .symbols = {
                {.path = "include/core/ssg/search.h",
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

    PaletteExecutionResult execute(std::string_view commandId) override {
        executed.emplace_back(commandId);
        if (commandId == "file.open") {
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

TEST(queryModesAreUnambiguousAndLinesAreValidated) {
    const ParsedSearchQuery file{.mode = SearchMode::File, .text = "file"};
    const ParsedSearchQuery symbol{.mode = SearchMode::Symbol, .text = "Widget"};
    const ParsedSearchQuery text{.mode = SearchMode::Text, .text = "needle"};
    const ParsedSearchQuery line{
        .mode = SearchMode::Line, .text = "42", .line = LineIndex{41}};
    ASSERT_EQ(WorkspaceSearcher{}.parse("file"), file);
    ASSERT_EQ(WorkspaceSearcher{}.parse("@Widget"), symbol);
    ASSERT_EQ(WorkspaceSearcher{}.parse("#needle"), text);
    ASSERT_EQ(WorkspaceSearcher{}.parse(":42"), line);
    ASSERT_EQ(WorkspaceSearcher{}.parse(":0").error,
              SearchQueryError::InvalidLine);
    ASSERT_EQ(WorkspaceSearcher{}.parse(":no").error,
              SearchQueryError::InvalidLine);

    const auto target = SearchNavigator{}.gotoLine(
        "src/search.cpp", WorkspaceSearcher{}.parse(":42"));
    ASSERT_TRUE(target.has_value());
    ASSERT_EQ(target->line, LineIndex{41});
    ASSERT_FALSE(SearchNavigator{}.gotoLine(
                     {}, WorkspaceSearcher{}.parse(":42"))
                     .has_value());
}

TEST(acceptedRankingGoldensMatch) {
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
        const auto results = WorkspaceSearcher{}.rank(
            snapshot, WorkspaceSearcher{}.parse(query), SearchCancellationToken{});
        ASSERT_EQ(labels(results), split(fields[2], ','));
    }
}

TEST(cancellationSupersessionAndStaleRevisionAreRejected) {
    FixtureWorkspace workspace;
    FixtureCommands commands;
    SearchController controller{workspace, commands};

    const auto first = controller.beginWorkspaceSearch("#search", Revision{8});
    const auto second = controller.beginWorkspaceSearch("#cancel", Revision{8});
    ASSERT_TRUE(first.cancellation.cancelled());

    const auto cancelled = controller.evaluate(first);
    ASSERT_TRUE(cancelled.cancelled);
    ASSERT_EQ(controller.publish(cancelled, Revision{8}),
              SearchPublishResult::Cancelled);

    auto superseded = controller.evaluate(second);
    superseded.generation = first.generation;
    ASSERT_EQ(controller.publish(superseded, Revision{8}),
              SearchPublishResult::Superseded);

    const auto completed = controller.evaluate(second);
    ASSERT_EQ(controller.publish(completed, Revision{9}),
              SearchPublishResult::StaleRevision);
    ASSERT_EQ(controller.publish(completed, Revision{8}),
              SearchPublishResult::Accepted);
    ASSERT_EQ(labels(controller.viewState().results),
              std::vector<std::string>{"src/search.cpp:3"});
}

TEST(navigationHistoryMatchesTransitionTable) {
    const NavigationTarget a{.path = "a.cpp", .line = LineIndex{1}};
    const NavigationTarget b{.path = "b.cpp", .line = LineIndex{2}};
    const NavigationTarget c{.path = "c.cpp", .line = LineIndex{3}};
    NavigationHistory history{3};

    const auto visitA = history.visit(a, NavigationOrigin::User);
    const NavigationTransition toA{.target = a,
                                    .pauseFollowEdits = true,
                                    .revealPrimaryCaret = true};
    const NavigationTransition toB{.target = b,
                                    .pauseFollowEdits = true,
                                    .revealPrimaryCaret = true};
    ASSERT_EQ(visitA, toA);
    const auto visitB = history.visit(b, NavigationOrigin::Programmatic);
    ASSERT_FALSE(visitB.pauseFollowEdits);
    ASSERT_EQ(history.back(), toA);
    ASSERT_EQ(history.forward(), toB);
    const auto ignoredBack = history.back();
    const auto visitC = history.visit(c, NavigationOrigin::User);
    ASSERT_TRUE(ignoredBack.target.has_value());
    ASSERT_TRUE(visitC.pauseFollowEdits);
    ASSERT_FALSE(history.forward().target.has_value());
    ASSERT_EQ(history.back().target, std::optional<NavigationTarget>{a});
    ASSERT_FALSE(history.back().target.has_value());

    NavigationHistory bounded{2};
    const auto ignoredA = bounded.visit(a, NavigationOrigin::User);
    const auto ignoredB = bounded.visit(b, NavigationOrigin::User);
    const auto ignoredC = bounded.visit(c, NavigationOrigin::User);
    ASSERT_TRUE(ignoredA.target.has_value());
    ASSERT_TRUE(ignoredB.target.has_value());
    ASSERT_TRUE(ignoredC.target.has_value());
    ASSERT_EQ(bounded.back().target, std::optional<NavigationTarget>{b});
    ASSERT_FALSE(bounded.back().target.has_value());
    ASSERT_THROWS(NavigationHistory{0}, std::invalid_argument);
}

TEST(paletteUsesInjectedCatalogAndDispatch) {
    FixtureWorkspace workspace;
    FixtureCommands commands;
    SearchController controller{workspace, commands};

    controller.openPalette(Revision{11});
    controller.updatePaletteQuery("open f", Revision{12});
    ASSERT_TRUE(controller.viewState().paletteOpen);
    ASSERT_EQ(labels(controller.viewState().results),
              std::vector<std::string>{"Open File"});
    ASSERT_EQ(controller.executePalette().accepted, true);
    ASSERT_EQ(commands.executed, std::vector<std::string>{"file.open"});

    controller.closePalette(Revision{13});
    ASSERT_FALSE(controller.executePalette().accepted);
}

TEST(commandExportsAreExact) {
    const auto set = searchCommandSet();
    const std::vector<std::string_view> expected{
        "palette.open",          "palette.close",        "palette.next",
        "palette.previous",      "palette.execute",      "file_finder.open",
        "file_finder.toggle_gitignore",                  "goto.file",
        "goto.line",             "goto.symbol",          "goto.back",
        "goto.forward",          "search.workspace",     "search.results_next",
        "search.results_previous"};
    std::vector<std::string_view> actual;
    for (const auto& descriptor : set.descriptors()) {
        actual.push_back(descriptor.id);
        ASSERT_TRUE(descriptor.userNavigation ==
                        (descriptor.id.starts_with("goto.") ||
                         descriptor.id.starts_with("search.results_")));
    }
    ASSERT_EQ(actual, expected);
}

} // namespace

SSG_TEST_SUITE(test_search) {
    RUN(queryModesAreUnambiguousAndLinesAreValidated);
    RUN(acceptedRankingGoldensMatch);
    RUN(cancellationSupersessionAndStaleRevisionAreRejected);
    RUN(navigationHistoryMatchesTransitionTable);
    RUN(paletteUsesInjectedCatalogAndDispatch);
    RUN(commandExportsAreExact);
    return failed == 0 ? 0 : 1;
}
