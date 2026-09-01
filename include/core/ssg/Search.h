#pragma once

#include <ssg/types.h>
#include <ssg/detail/generated/semantic_wire_manifest.h>

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

enum class SearchMode : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_SEARCH_MODE_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};

// The closed set of SearchMode values, in wire order. The single domain every codec
// validates against, so adding a mode cannot leave one decoder accepting it and
// another rejecting it.
inline constexpr std::array kAllSearchModes{
#define SSG_ENUMERATOR(symbol, ordinal) SearchMode::symbol,
    SSG_SEARCH_MODE_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_SEARCH_MODE_ENUMERATORS

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
    virtual PaletteExecutionResult execute(std::string_view commandId) = 0;
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
    Revision sourceRevision{0};
    ParsedSearchQuery query;
    SearchCancellationToken cancellation;
};

struct WorkspaceSearchBatch {
    std::uint64_t generation = 0;
    Revision sourceRevision{0};
    std::vector<SearchResult> results;
    bool cancelled = false;
};

class WorkspaceSearcher {
public:
    [[nodiscard]] ParsedSearchQuery parse(std::string_view query) const;
    [[nodiscard]] std::vector<SearchResult> rank(
        const WorkspaceSnapshot& workspace, const ParsedSearchQuery& query,
        const SearchCancellationToken& cancellation) const;
    [[nodiscard]] WorkspaceSearchBatch evaluate(
        const SearchWorkspaceSource& source,
        const WorkspaceSearchRequest& request) const;
};

enum class NavigationOrigin : std::uint8_t { User, Programmatic };

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
    bool pauseFollowEdits = false;
    bool revealPrimaryCaret = false;
    friend bool operator==(const NavigationTransition&,
                           const NavigationTransition&) = default;
};

class SearchNavigator {
public:
    [[nodiscard]] std::optional<NavigationTarget> target(
        const SearchResult& result) const;
    [[nodiscard]] std::optional<NavigationTarget> gotoLine(
        std::string path, const ParsedSearchQuery& query) const;
};

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
    SearchController(const SearchWorkspaceSource& workspace,
                     SearchCommandSource& commands) noexcept;

    void openPalette(Revision revision);
    void closePalette(Revision revision);
    void updatePaletteQuery(std::string query, Revision revision);
    void selectNext();
    void selectPrevious();
    [[nodiscard]] PaletteExecutionResult executePalette();

    [[nodiscard]] WorkspaceSearchRequest beginWorkspaceSearch(
        std::string query, Revision sourceRevision);
    [[nodiscard]] WorkspaceSearchBatch evaluate(
        const WorkspaceSearchRequest& request) const;
    void cancelWorkspaceSearch() noexcept;
    [[nodiscard]] SearchPublishResult publish(
        const WorkspaceSearchBatch& batch, Revision currentRevision);

    [[nodiscard]] const SearchViewState& viewState() const noexcept {
        return state_;
    }

private:
    void rankPalette();

    const SearchWorkspaceSource& workspace_;
    SearchCommandSource& commands_;
    SearchViewState state_;
    std::optional<WorkspaceSearchRequest> activeRequest_;
};

struct SearchCommandDescriptorExport {
    std::string_view id;
    bool userNavigation = false;
    friend bool operator==(const SearchCommandDescriptorExport&,
                           const SearchCommandDescriptorExport&) = default;
};

class SearchCommandSet {
public:
    [[nodiscard]] const std::array<SearchCommandDescriptorExport, 15>&
    descriptors() const noexcept {
        return descriptors_;
    }

private:
    const std::array<SearchCommandDescriptorExport, 15> descriptors_{{
        {"palette.open", false},
        {"palette.close", false},
        {"palette.next", false},
        {"palette.previous", false},
        {"palette.execute", false},
        {"file_finder.open", false},
        {"file_finder.toggle_gitignore", false},
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

[[nodiscard]] SearchCommandSet searchCommandSet();

} // namespace ssg
