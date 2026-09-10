#include "test_helpers.h"

#include <ssg/Search.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ssg;

class NoIgnore final : public GitIgnoreMatcher {
public:
    [[nodiscard]] bool usable() const override { return false; }
    [[nodiscard]] bool ignores(
        const std::filesystem::path&) const override {
        return false;
    }
};

WorkspaceSnapshot fixtureWorkspace(std::uint64_t revision) {
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
    };
}

WorkspaceCorpus corpusFor(
    const WorkspaceSnapshot& snapshot,
    WorkspaceCorpusReader reader = readFile) {
    const auto root =
        std::filesystem::temp_directory_path() / "ssg-search-open-buffers";
    std::filesystem::create_directories(root);
    std::vector<WorkspaceCorpusBuffer> buffers;
    buffers.reserve(snapshot.files.size());
    for (const auto& file : snapshot.files) {
        buffers.push_back(
            {std::optional<std::string>{file.path},
             [text = file.text] {
                 return std::optional<std::string>{text};
             }});
    }
    NoIgnore ignore;
    WorkspaceCorpus corpus{root, std::move(buffers), ignore,
                           std::move(reader)};
    std::filesystem::remove_all(root);
    return corpus;
}

std::vector<SearchResult> rank(const WorkspaceCorpus& corpus,
                               std::string_view query) {
    WorkspaceSearchState state{
        .request = WorkspaceSearchRequest{
            .query = parseWorkspaceSearchQuery(query)}};
    while (!rankWorkspaceSearch(
        corpus, state, std::numeric_limits<std::uint64_t>::max())) {
    }
    return state.results;
}

SearchCommands fixtureCommands(std::vector<std::string>& executed) {
    return {
        .descriptors = [] {
        return std::vector<SearchCommandDescriptor>{
            {.id = "file.open", .label = "Open File"},
            {.id = "palette.close", .label = "Close Command Palette"},
            {.id = "workspace.open", .label = "Open Workspace"},
        };
        },
        .execute = [&executed](std::string_view commandId) {
        executed.emplace_back(commandId);
        if (commandId == "file.open") {
            return PaletteExecutionResult{.accepted = true};
        }
        return PaletteExecutionResult{
            .accepted = false, .message = "command rejected"};
        },
    };
}

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
    ASSERT_EQ(parseWorkspaceSearchQuery("file"), file);
    ASSERT_EQ(parseWorkspaceSearchQuery("@Widget"), symbol);
    ASSERT_EQ(parseWorkspaceSearchQuery("#needle"), text);
    ASSERT_EQ(parseWorkspaceSearchQuery(":42"), line);
    ASSERT_EQ(parseWorkspaceSearchQuery(":0").error,
              SearchQueryError::InvalidLine);
    ASSERT_EQ(parseWorkspaceSearchQuery(":no").error,
              SearchQueryError::InvalidLine);

    const auto target = searchGotoLine(
        "src/search.cpp", parseWorkspaceSearchQuery(":42"));
    ASSERT_TRUE(target.has_value());
    ASSERT_EQ(target->line, LineIndex{41});
    ASSERT_FALSE(searchGotoLine(
                     {}, parseWorkspaceSearchQuery(":42"))
                     .has_value());
}

TEST(acceptedRankingGoldensMatch) {
    const auto snapshot = fixtureWorkspace(std::uint64_t{7});
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
        const auto corpus = corpusFor(snapshot);
        const auto results = rank(corpus, query);
        const auto expected =
            fields[2] == "-" ? std::vector<std::string>{}
                             : split(fields[2], ',');
        ASSERT_EQ(labels(results), expected);
    }
}

TEST(cancellationSupersessionAndStaleRevisionAreRejected) {
    std::vector<std::string> executed;
    SearchController controller{fixtureCommands(executed)};

    auto first =
        controller.beginWorkspaceSearch("#search", std::uint64_t{8});
    auto second =
        controller.beginWorkspaceSearch("#cancel", std::uint64_t{8});
    ASSERT_TRUE(first.request.cancellation.cancelled());

    const auto firstCorpus =
        corpusFor(fixtureWorkspace(first.request.sourceRevision));
    const auto cancelled = controller.evaluate(
        first, firstCorpus, std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(cancelled.cancelled);
    ASSERT_EQ(controller.publish(cancelled, std::uint64_t{8}),
              SearchPublishResult::Cancelled);

    const auto secondCorpus =
        corpusFor(fixtureWorkspace(second.request.sourceRevision));
    auto superseded = controller.evaluate(
        second, secondCorpus, std::numeric_limits<std::uint64_t>::max());
    superseded.generation = first.request.generation;
    ASSERT_EQ(controller.publish(superseded, std::uint64_t{8}),
              SearchPublishResult::Superseded);

    second = controller.beginWorkspaceSearch("#cancel", std::uint64_t{8});
    const auto completed = controller.evaluate(
        second, secondCorpus, std::numeric_limits<std::uint64_t>::max());
    ASSERT_EQ(controller.publish(completed, std::uint64_t{9}),
              SearchPublishResult::StaleRevision);
    ASSERT_EQ(controller.publish(completed, std::uint64_t{8}),
              SearchPublishResult::Accepted);
    ASSERT_EQ(labels(controller.viewState().results),
              std::vector<std::string>{"src/search.cpp:3"});
}

TEST(slicedTextSearchMatchesUnslicedAndStopsWhenCancelled) {
    const auto root =
        std::filesystem::temp_directory_path() / "ssg-search-slices";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    for (int index = 0; index < 4; ++index) {
        std::ofstream{root / ("file-" + std::to_string(index) + ".txt")}
            << "needle\n";
    }
    NoIgnore ignore;
    std::size_t reads = 0;
    WorkspaceCorpus corpus{
        root, {}, ignore,
        [&](const std::filesystem::path& path) {
            ++reads;
            return readFile(path);
        }};
    WorkspaceSearchState sliced{
        .request = WorkspaceSearchRequest{
            .query = parseWorkspaceSearchQuery("#needle")}};
    ASSERT_FALSE(rankWorkspaceSearch(corpus, sliced, 1));
    ASSERT_EQ(reads, std::size_t{1});
    ASSERT_FALSE(rankWorkspaceSearch(corpus, sliced, 1));
    ASSERT_EQ(reads, std::size_t{2});

    const auto unsliced = rank(corpus, "#needle");
    while (!rankWorkspaceSearch(corpus, sliced, 1)) {
    }
    ASSERT_EQ(sliced.results, unsliced);

    reads = 0;
    WorkspaceSearchState cancelled{
        .request = WorkspaceSearchRequest{
            .query = parseWorkspaceSearchQuery("#needle")}};
    ASSERT_FALSE(rankWorkspaceSearch(corpus, cancelled, 1));
    cancelled.request.cancellation.cancel();
    ASSERT_TRUE(rankWorkspaceSearch(corpus, cancelled, 1));
    ASSERT_EQ(reads, std::size_t{1});
    std::filesystem::remove_all(root);
}

TEST(pathOnlyModesNeverReadFileContents) {
    const auto snapshot = fixtureWorkspace(1);
    std::size_t reads = 0;
    const auto corpus = corpusFor(
        snapshot,
        [&](const std::filesystem::path&) -> FileReadResult {
            ++reads;
            return {};
        });
    ASSERT_FALSE(rank(corpus, "search").empty());
    ASSERT_TRUE(rank(corpus, "@Search").empty());
    ASSERT_EQ(reads, std::size_t{0});
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

TEST(navigationHistoryPeekBackDoesNotMutateCursor) {
    const NavigationTarget a{.path = "a.cpp", .line = LineIndex{1}};
    const NavigationTarget b{.path = "b.cpp", .line = LineIndex{2}};
    const NavigationTransition toA{.target = a,
                                    .pauseFollowEdits = true,
                                    .revealPrimaryCaret = true};
    NavigationHistory history{4};
    const auto visitA = history.visit(a, NavigationOrigin::User);
    const auto visitB = history.visit(b, NavigationOrigin::User);
    ASSERT_TRUE(visitA.target.has_value());
    ASSERT_TRUE(visitB.target.has_value());

    // Repeated peeks answer the same thing: the cursor is still on b.
    ASSERT_EQ(history.peekBack(), toA);
    ASSERT_EQ(history.peekBack(), toA);
    ASSERT_FALSE(history.peekForward().target.has_value());

    // Committing with back() is what moves the cursor.
    ASSERT_EQ(history.back(), toA);
    ASSERT_FALSE(history.peekBack().target.has_value());
}

TEST(navigationHistoryPeekForwardDoesNotMutateCursor) {
    const NavigationTarget a{.path = "a.cpp", .line = LineIndex{1}};
    const NavigationTarget b{.path = "b.cpp", .line = LineIndex{2}};
    const NavigationTransition toB{.target = b,
                                    .pauseFollowEdits = true,
                                    .revealPrimaryCaret = true};
    NavigationHistory history{4};
    const auto visitA = history.visit(a, NavigationOrigin::User);
    const auto visitB = history.visit(b, NavigationOrigin::User);
    ASSERT_TRUE(visitA.target.has_value());
    ASSERT_TRUE(visitB.target.has_value());
    ASSERT_EQ(history.back().target, std::optional<NavigationTarget>{a});

    ASSERT_EQ(history.peekForward(), toB);
    ASSERT_EQ(history.peekForward(), toB);
    ASSERT_FALSE(history.peekBack().target.has_value());

    ASSERT_EQ(history.forward(), toB);
    ASSERT_FALSE(history.peekForward().target.has_value());
}

TEST(paletteUsesInjectedCatalogAndDispatch) {
    std::vector<std::string> executed;
    SearchController controller{fixtureCommands(executed)};

    controller.openPalette(std::uint64_t{11});
    controller.updatePaletteQuery("open f", std::uint64_t{12});
    ASSERT_TRUE(controller.viewState().paletteOpen);
    ASSERT_EQ(labels(controller.viewState().results),
              std::vector<std::string>{"Open File"});
    ASSERT_EQ(controller.executePalette().accepted, true);
    ASSERT_EQ(executed, std::vector<std::string>{"file.open"});

    controller.closePalette(std::uint64_t{13});
    ASSERT_FALSE(controller.executePalette().accepted);
}

} // namespace

SSG_TEST_SUITE(test_search) {
    RUN(queryModesAreUnambiguousAndLinesAreValidated);
    RUN(acceptedRankingGoldensMatch);
    RUN(cancellationSupersessionAndStaleRevisionAreRejected);
    RUN(slicedTextSearchMatchesUnslicedAndStopsWhenCancelled);
    RUN(pathOnlyModesNeverReadFileContents);
    RUN(navigationHistoryMatchesTransitionTable);
    RUN(navigationHistoryPeekBackDoesNotMutateCursor);
    RUN(navigationHistoryPeekForwardDoesNotMutateCursor);
    RUN(paletteUsesInjectedCatalogAndDispatch);
    return failed == 0 ? 0 : 1;
}
