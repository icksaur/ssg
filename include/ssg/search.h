#pragma once

#include <ssg/types.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

enum class SearchMode : std::uint8_t { file, line, symbol, text, command };
enum class SearchQueryError : std::uint8_t { none, invalid_line };

struct ParsedSearchQuery {
    SearchMode mode = SearchMode::file;
    std::string text;
    std::optional<LineIndex> line;
    SearchQueryError error = SearchQueryError::none;
    friend bool operator==(const ParsedSearchQuery&,
                           const ParsedSearchQuery&) = default;
};

[[nodiscard]] ParsedSearchQuery parse_search_query(std::string_view query);

struct WorkspaceFile {
    std::string path;
    std::string text;
    friend bool operator==(const WorkspaceFile&, const WorkspaceFile&) = default;
};

struct WorkspaceSymbol {
    std::string path;
    std::string name;
    std::size_t line = 1;
    std::size_t column = 1;
    friend bool operator==(const WorkspaceSymbol&,
                           const WorkspaceSymbol&) = default;
};

struct WorkspaceSnapshot {
    Revision revision{0};
    std::vector<WorkspaceFile> files;
    std::vector<WorkspaceSymbol> symbols;
    friend bool operator==(const WorkspaceSnapshot&,
                           const WorkspaceSnapshot&) = default;
};

class SearchWorkspaceSource {
public:
    virtual ~SearchWorkspaceSource() = default;
    [[nodiscard]] virtual WorkspaceSnapshot snapshot(
        Revision revision) const = 0;
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

class SearchCommandSource {
public:
    virtual ~SearchCommandSource() = default;
    [[nodiscard]] virtual std::vector<SearchCommandDescriptor> descriptors()
        const = 0;
    virtual PaletteExecutionResult execute(std::string_view command_id) = 0;
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
    SearchMode mode = SearchMode::file;
    std::string path;
    std::string label;
    std::optional<LineIndex> line;
    std::size_t column = 1;
    int score = 0;
    friend bool operator==(const SearchResult&, const SearchResult&) = default;
};

[[nodiscard]] std::vector<SearchResult> rank_workspace(
    const WorkspaceSnapshot& workspace, const ParsedSearchQuery& query,
    const SearchCancellationToken& cancellation);

struct WorkspaceSearchRequest {
    std::uint64_t generation = 0;
    Revision source_revision{0};
    ParsedSearchQuery query;
    SearchCancellationToken cancellation;
};

struct WorkspaceSearchBatch {
    std::uint64_t generation = 0;
    Revision source_revision{0};
    std::vector<SearchResult> results;
    bool cancelled = false;
};

[[nodiscard]] WorkspaceSearchBatch evaluate_workspace_search(
    const SearchWorkspaceSource& source,
    const WorkspaceSearchRequest& request);

enum class NavigationOrigin : std::uint8_t { user, programmatic };

struct NavigationTarget {
    std::string path;
    LineIndex line{0};
    std::size_t column = 1;
    std::optional<std::string> symbol;
    friend bool operator==(const NavigationTarget&,
                           const NavigationTarget&) = default;
};

struct NavigationTransition {
    std::optional<NavigationTarget> target;
    bool pause_follow_edits = false;
    bool reveal_primary_caret = false;
    friend bool operator==(const NavigationTransition&,
                           const NavigationTransition&) = default;
};

[[nodiscard]] std::optional<NavigationTarget> navigation_target(
    const SearchResult& result);
[[nodiscard]] std::optional<NavigationTarget> goto_line(
    std::string path, const ParsedSearchQuery& query);

class NavigationHistory {
public:
    explicit NavigationHistory(std::size_t capacity);
    [[nodiscard]] NavigationTransition visit(
        NavigationTarget target, NavigationOrigin origin);
    [[nodiscard]] NavigationTransition back();
    [[nodiscard]] NavigationTransition forward();

private:
    [[nodiscard]] static NavigationTransition transition(
        const NavigationTarget& target, NavigationOrigin origin);

    std::size_t capacity_;
    std::vector<NavigationTarget> entries_;
    std::optional<std::size_t> cursor_;
};

struct SearchViewState {
    Revision revision{0};
    bool palette_open = false;
    std::string query;
    SearchMode mode = SearchMode::file;
    std::vector<SearchResult> results;
    std::optional<std::size_t> selected_index;
    std::uint64_t search_generation = 0;
    bool searching = false;
    friend bool operator==(const SearchViewState&,
                           const SearchViewState&) = default;
};

enum class SearchPublishResult : std::uint8_t {
    accepted,
    cancelled,
    superseded,
    stale_revision,
};

class SearchController {
public:
    SearchController(const SearchWorkspaceSource& workspace,
                     SearchCommandSource& commands) noexcept;

    void open_palette(Revision revision);
    void close_palette(Revision revision);
    void update_palette_query(std::string query, Revision revision);
    void select_next();
    void select_previous();
    [[nodiscard]] PaletteExecutionResult execute_palette();

    [[nodiscard]] WorkspaceSearchRequest begin_workspace_search(
        std::string query, Revision source_revision);
    [[nodiscard]] WorkspaceSearchBatch evaluate(
        const WorkspaceSearchRequest& request) const;
    void cancel_workspace_search() noexcept;
    [[nodiscard]] SearchPublishResult publish(
        const WorkspaceSearchBatch& batch, Revision current_revision);

    [[nodiscard]] const SearchViewState& view_state() const noexcept {
        return state_;
    }

private:
    void rank_palette();

    const SearchWorkspaceSource& workspace_;
    SearchCommandSource& commands_;
    SearchViewState state_;
    std::optional<WorkspaceSearchRequest> active_request_;
};

struct SearchCommandDescriptorExport {
    std::string_view id;
    bool user_navigation = false;
    friend bool operator==(const SearchCommandDescriptorExport&,
                           const SearchCommandDescriptorExport&) = default;
};

class SearchCommandSet {
public:
    [[nodiscard]] const std::array<SearchCommandDescriptorExport, 13>&
    descriptors() const noexcept {
        return descriptors_;
    }

private:
    const std::array<SearchCommandDescriptorExport, 13> descriptors_{{
        {"palette.open", false},
        {"palette.close", false},
        {"palette.next", false},
        {"palette.previous", false},
        {"palette.execute", false},
        {"goto.file", true},
        {"goto.line", true},
        {"goto.symbol", true},
        {"goto.back", true},
        {"goto.forward", true},
        {"search.workspace", false},
        {"search.results_next", true},
        {"search.results_previous", true},
    }};
};

[[nodiscard]] SearchCommandSet search_command_set();

struct SearchDelta {
    Revision base_revision{0};
    Revision revision{0};
    std::optional<SearchViewState> state;
    friend bool operator==(const SearchDelta&, const SearchDelta&) = default;
};

[[nodiscard]] SearchDelta derive_search_delta(const SearchViewState& base,
                                              const SearchViewState& target);

enum class SearchReplayError : std::uint8_t {
    none,
    stale_revision,
    malformed_delta,
};

struct SearchReplayResult {
    std::optional<SearchViewState> state;
    SearchReplayError error = SearchReplayError::none;
    [[nodiscard]] bool accepted() const noexcept { return state.has_value(); }
};

[[nodiscard]] SearchReplayResult replay_search_delta(
    const SearchViewState& base, const SearchDelta& delta);

} // namespace ssg
