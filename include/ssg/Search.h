#pragma once

#include <ssg/WorkspaceCorpus.h>
#include <ssg/types.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class SearchMode : std::uint8_t {
    File = 0,
    Line = 1,
    Symbol = 2,
    Text = 3,
    Command = 4,
};

// The closed set of SearchMode values, in wire order. The single domain every codec
// validates against, so adding a mode cannot leave one decoder accepting it and
// another rejecting it.
inline constexpr std::array kAllSearchModes{
    SearchMode::File,
    SearchMode::Line,
    SearchMode::Symbol,
    SearchMode::Text,
    SearchMode::Command,
};

enum class SearchQueryError : std::uint8_t { None, InvalidLine };

struct ParsedSearchQuery {
    SearchMode mode = SearchMode::File;
    std::string text;
    std::optional<LineIndex> line;
    SearchQueryError error = SearchQueryError::None;
    friend bool operator==(const ParsedSearchQuery&,
                           const ParsedSearchQuery&) = default;
};

struct WorkspaceFile {
    std::string path;
    std::string text;
    friend bool operator==(const WorkspaceFile&, const WorkspaceFile&) = default;
};

struct WorkspaceSnapshot {
    // Replace preview is intentionally the sole eager consumer. Interactive
    // workspace search reads through WorkspaceCorpus instead.
    std::uint64_t revision{0};
    std::vector<WorkspaceFile> files;
    friend bool operator==(const WorkspaceSnapshot&,
                           const WorkspaceSnapshot&) = default;
};

struct SearchCommandDescriptor {
    std::string id;
    std::string label;
    friend bool operator==(const SearchCommandDescriptor&,
                           const SearchCommandDescriptor&) = default;
};

struct PaletteExecutionResult {
    bool accepted = false;
    std::string message;
    friend bool operator==(const PaletteExecutionResult&,
                           const PaletteExecutionResult&) = default;
};

struct SearchCommands {
    std::function<std::vector<SearchCommandDescriptor>()> descriptors;
    std::function<PaletteExecutionResult(std::string_view)> execute;
};

class SearchCancellationToken {
public:
    SearchCancellationToken();
    [[nodiscard]] bool cancelled() const noexcept;
    void cancel() const noexcept;

private:
    std::shared_ptr<std::atomic_bool> cancelled_;
};

struct SearchResult {
    SearchMode mode = SearchMode::File;
    std::string path;
    std::string label;
    std::optional<LineIndex> line;
    std::size_t column = 1;
    int score = 0;
    friend bool operator==(const SearchResult&, const SearchResult&) = default;
};

struct WorkspaceSearchRequest {
    std::uint64_t generation = 0;
    std::uint64_t sourceRevision{0};
    ParsedSearchQuery query;
    SearchCancellationToken cancellation;
};

struct WorkspaceSearchBatch {
    std::uint64_t generation = 0;
    std::uint64_t sourceRevision{0};
    // Partial batches carry no results; the accumulated set is published only
    // when finished is true.
    std::vector<SearchResult> results;
    bool cancelled = false;
    bool finished = false;
};

struct WorkspaceSearchState {
    WorkspaceSearchRequest request;
    std::size_t cursor = 0;
    std::vector<SearchResult> results;
    bool finished = false;
};

[[nodiscard]] ParsedSearchQuery parseWorkspaceSearchQuery(
    std::string_view query);
[[nodiscard]] bool rankWorkspaceSearch(
    const WorkspaceCorpus& corpus, WorkspaceSearchState& state,
    std::uint64_t workBudget);
[[nodiscard]] WorkspaceSearchBatch evaluateWorkspaceSearch(
    const WorkspaceCorpus& corpus, WorkspaceSearchState& state,
    std::uint64_t workBudget);

enum class NavigationOrigin : std::uint8_t { User, Programmatic };

struct NavigationTarget {
    std::string path;
    LineIndex line{0};
    // One-based byte position within the line; navigation snaps it to a
    // grapheme boundary before constructing a caret.
    std::size_t column = 1;
    std::optional<std::string> symbol;
    friend bool operator==(const NavigationTarget&,
                           const NavigationTarget&) = default;
};

struct NavigationTransition {
    std::optional<NavigationTarget> target;
    bool pauseFollowEdits = false;
    bool revealPrimaryCaret = false;
    friend bool operator==(const NavigationTransition&,
                           const NavigationTransition&) = default;
};

// The single source of the transition policy: what a visit/back/forward to
// `target` asks the editor to do. Shared by NavigationHistory and by the
// goto.* handlers, which build a transition before any history is recorded.
[[nodiscard]] NavigationTransition navigationTransition(
    const NavigationTarget& target, NavigationOrigin origin);

[[nodiscard]] std::optional<NavigationTarget> searchNavigationTarget(
    const SearchResult& result);
[[nodiscard]] std::optional<NavigationTarget> searchGotoLine(
    std::string path, const ParsedSearchQuery& query);

class NavigationHistory {
public:
    explicit NavigationHistory(std::size_t capacity);
    [[nodiscard]] NavigationTransition visit(
        NavigationTarget target, NavigationOrigin origin);
    [[nodiscard]] NavigationTransition back();
    [[nodiscard]] NavigationTransition forward();
    // The transition back()/forward() would return, without moving the cursor.
    // Navigation must validate and apply before it records, so the caller peeks,
    // applies, and only then commits with back()/forward().
    [[nodiscard]] NavigationTransition peekBack() const;
    [[nodiscard]] NavigationTransition peekForward() const;

private:
    std::size_t capacity_;
    std::vector<NavigationTarget> entries_;
    std::optional<std::size_t> cursor_;
};

struct SearchViewState {
    std::uint64_t revision{0};
    bool paletteOpen = false;
    std::string query;
    SearchMode mode = SearchMode::File;
    std::vector<SearchResult> results;
    std::optional<std::size_t> selectedIndex;
    std::uint64_t searchGeneration = 0;
    bool searching = false;
    friend bool operator==(const SearchViewState&,
                           const SearchViewState&) = default;
};

enum class SearchPublishResult : std::uint8_t {
    Accepted,
    Cancelled,
    Superseded,
    StaleRevision,
};

class SearchController {
public:
    explicit SearchController(SearchCommands commands);

    void openPalette(std::uint64_t revision);
    void closePalette(std::uint64_t revision);
    void updatePaletteQuery(std::string query, std::uint64_t revision);
    void selectNext();
    void selectPrevious();
    [[nodiscard]] PaletteExecutionResult executePalette();

    [[nodiscard]] WorkspaceSearchState beginWorkspaceSearch(
        std::string query, std::uint64_t sourceRevision);
    [[nodiscard]] WorkspaceSearchState beginWorkspaceSearch(
        ParsedSearchQuery query, std::uint64_t sourceRevision);
    [[nodiscard]] WorkspaceSearchBatch evaluate(
        WorkspaceSearchState& state, const WorkspaceCorpus& corpus,
        std::uint64_t workBudget) const;
    void cancelWorkspaceSearch() noexcept;
    [[nodiscard]] SearchPublishResult publish(
        const WorkspaceSearchBatch& batch, std::uint64_t currentRevision);

    [[nodiscard]] const SearchViewState& viewState() const noexcept {
        return state_;
    }

private:
    [[nodiscard]] WorkspaceSearchState beginWorkspaceSearch(
        ParsedSearchQuery query, std::string displayQuery,
        std::uint64_t sourceRevision);
    void rankPalette();

    SearchCommands commands_;
    SearchViewState state_;
    std::optional<WorkspaceSearchRequest> activeRequest_;
};

} // namespace ssg
