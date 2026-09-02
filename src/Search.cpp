#include <ssg/Search.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

char folded(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

std::optional<int> fuzzyScore(std::string_view candidate,
                               std::string_view query) {
    if (query.empty()) {
        return 0;
    }

    int score = 0;
    std::size_t cursor = 0;
    std::size_t previous = std::string_view::npos;
    for (const char wanted : query) {
        const char needle = folded(wanted);
        while (cursor < candidate.size() && folded(candidate[cursor]) != needle) {
            ++cursor;
        }
        if (cursor == candidate.size()) {
            return std::nullopt;
        }
        score += 10;
        if (cursor == 0 || candidate[cursor - 1] == '/' ||
            candidate[cursor - 1] == '_' || candidate[cursor - 1] == '-' ||
            candidate[cursor - 1] == '.') {
            score += 8;
        }
        if (previous != std::string_view::npos && cursor == previous + 1) {
            score += 6;
        }
        if (candidate[cursor] == wanted) {
            ++score;
        }
        previous = cursor++;
    }
    score -= static_cast<int>(
        std::min<std::size_t>(candidate.size(), std::size_t{100}));
    return score;
}

std::vector<SearchResult> rankFiles(const WorkspaceSnapshot& workspace,
                                     std::string_view query,
                                     const SearchCancellationToken& token) {
    std::vector<SearchResult> results;
    for (const auto& file : workspace.files) {
        if (token.cancelled()) {
            return {};
        }
        const auto score = fuzzyScore(file.path, query);
        if (score) {
            results.push_back({.mode = SearchMode::File,
                               .path = file.path,
                               .label = file.path,
                               .score = *score});
        }
    }
    std::ranges::sort(results, [](const auto& left, const auto& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.path < right.path;
    });
    return results;
}

std::vector<SearchResult> rankSymbols(const WorkspaceSnapshot& workspace,
                                       std::string_view query,
                                       const SearchCancellationToken& token) {
    std::vector<SearchResult> results;
    for (const auto& symbol : workspace.symbols) {
        if (token.cancelled()) {
            return {};
        }
        const auto score = fuzzyScore(symbol.name, query);
        if (score) {
            results.push_back(
                {.mode = SearchMode::Symbol,
                 .path = symbol.path,
                 .label = symbol.path + ":" + symbol.name,
                 .line = LineIndex{symbol.line == 0 ? 0 : symbol.line - 1},
                 .column = symbol.column,
                 .score = *score});
        }
    }
    std::ranges::sort(results, [](const auto& left, const auto& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        const auto leftName = left.label.substr(left.label.find(':') + 1);
        const auto rightName = right.label.substr(right.label.find(':') + 1);
        if (leftName != rightName) {
            return leftName < rightName;
        }
        return left.path < right.path;
    });
    return results;
}

std::vector<SearchResult> rankText(const WorkspaceSnapshot& workspace,
                                    std::string_view query,
                                    const SearchCancellationToken& token) {
    std::vector<SearchResult> results;
    if (query.empty()) {
        return results;
    }
    for (const auto& file : workspace.files) {
        std::size_t lineStart = 0;
        std::size_t lineNumber = 0;
        while (lineStart <= file.text.size()) {
            if (token.cancelled()) {
                return {};
            }
            const auto lineEnd = file.text.find('\n', lineStart);
            const auto line = std::string_view{file.text}.substr(
                lineStart, lineEnd == std::string::npos
                                ? file.text.size() - lineStart
                                : lineEnd - lineStart);
            const auto match = line.find(query);
            if (match != std::string_view::npos) {
                results.push_back(
                    {.mode = SearchMode::Text,
                     .path = file.path,
                     .label = file.path + ":" + std::to_string(lineNumber + 1),
                     .line = LineIndex{lineNumber},
                     .column = match + 1,
                     .score = 0});
            }
            if (lineEnd == std::string::npos) {
                break;
            }
            lineStart = lineEnd + 1;
            ++lineNumber;
        }
    }
    std::ranges::sort(results, [](const auto& left, const auto& right) {
        if (left.path != right.path) {
            return left.path < right.path;
        }
        return left.line < right.line;
    });
    return results;
}

NavigationTransition emptyTransition() { return {}; }

} // namespace

ParsedSearchQuery WorkspaceSearcher::parse(std::string_view query) const {
    ParsedSearchQuery result;
    if (query.empty()) {
        return result;
    }
    switch (query.front()) {
    case '@':
        result.mode = SearchMode::Symbol;
        result.text = query.substr(1);
        return result;
    case '#':
        result.mode = SearchMode::Text;
        result.text = query.substr(1);
        return result;
    case ':': {
        result.mode = SearchMode::Line;
        result.text = query.substr(1);
        std::size_t oneBased = 0;
        const auto [end, error] = std::from_chars(
            result.text.data(), result.text.data() + result.text.size(), oneBased);
        if (result.text.empty() || error != std::errc{} ||
            end != result.text.data() + result.text.size() || oneBased == 0) {
            result.error = SearchQueryError::InvalidLine;
            return result;
        }
        result.line = LineIndex{oneBased - 1};
        return result;
    }
    default:
        result.text = query;
        return result;
    }
}

SearchCancellationToken::SearchCancellationToken()
    : cancelled_{std::make_shared<std::atomic_bool>(false)} {}

bool SearchCancellationToken::cancelled() const noexcept {
    return cancelled_->load(std::memory_order_relaxed);
}

void SearchCancellationToken::cancel() const noexcept {
    cancelled_->store(true, std::memory_order_relaxed);
}

std::vector<SearchResult> WorkspaceSearcher::rank(
    const WorkspaceSnapshot& workspace, const ParsedSearchQuery& query,
    const SearchCancellationToken& cancellation) const {
    if (query.error != SearchQueryError::None || cancellation.cancelled()) {
        return {};
    }
    switch (query.mode) {
    case SearchMode::File:
        return rankFiles(workspace, query.text, cancellation);
    case SearchMode::Symbol:
        return rankSymbols(workspace, query.text, cancellation);
    case SearchMode::Text:
        return rankText(workspace, query.text, cancellation);
    case SearchMode::Line:
    case SearchMode::Command:
        return {};
    }
    return {};
}

WorkspaceSearchBatch WorkspaceSearcher::evaluate(
    const SearchWorkspaceSource& source,
    const WorkspaceSearchRequest& request) const {
    WorkspaceSearchBatch batch{.generation = request.generation,
                               .sourceRevision = request.sourceRevision};
    if (request.cancellation.cancelled()) {
        batch.cancelled = true;
        return batch;
    }
    const auto workspace = source.snapshot(request.sourceRevision);
    batch.sourceRevision = workspace.revision;
    batch.results = rank(workspace, request.query, request.cancellation);
    batch.cancelled = request.cancellation.cancelled();
    return batch;
}

std::optional<NavigationTarget> SearchNavigator::target(
    const SearchResult& result) const {
    if (result.path.empty()) {
        return std::nullopt;
    }
    return NavigationTarget{.path = result.path,
                            .line = result.line.value_or(LineIndex{0}),
                            .column = result.column,
                            .symbol = result.mode == SearchMode::Symbol
                                          ? std::optional<std::string>{
                                                result.label.substr(
                                                    result.label.find(':') + 1)}
                                          : std::nullopt};
}

std::optional<NavigationTarget> SearchNavigator::gotoLine(
    std::string path, const ParsedSearchQuery& query) const {
    if (path.empty() || query.mode != SearchMode::Line ||
        query.error != SearchQueryError::None || !query.line) {
        return std::nullopt;
    }
    return NavigationTarget{
        .path = std::move(path), .line = *query.line, .column = 1};
}

NavigationHistory::NavigationHistory(std::size_t capacity)
    : capacity_{capacity} {
    if (capacity == 0) {
        throw std::invalid_argument{"navigation history capacity must be nonzero"};
    }
}

NavigationTransition NavigationHistory::transition(
    const NavigationTarget& target, NavigationOrigin origin) {
    return {.target = target,
            .pauseFollowEdits = origin == NavigationOrigin::User,
            .revealPrimaryCaret = true};
}

NavigationTransition NavigationHistory::visit(
    NavigationTarget target, NavigationOrigin origin) {
    if (cursor_ && entries_[*cursor_] == target) {
        return transition(target, origin);
    }
    if (cursor_ && *cursor_ + 1 < entries_.size()) {
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(*cursor_ + 1),
                       entries_.end());
    }
    entries_.push_back(std::move(target));
    if (entries_.size() > capacity_) {
        entries_.erase(entries_.begin());
    }
    cursor_ = entries_.size() - 1;
    return transition(entries_.back(), origin);
}

NavigationTransition NavigationHistory::back() {
    if (!cursor_ || *cursor_ == 0) {
        return emptyTransition();
    }
    --*cursor_;
    return transition(entries_[*cursor_], NavigationOrigin::User);
}

NavigationTransition NavigationHistory::forward() {
    if (!cursor_ || *cursor_ + 1 >= entries_.size()) {
        return emptyTransition();
    }
    ++*cursor_;
    return transition(entries_[*cursor_], NavigationOrigin::User);
}

SearchController::SearchController(const SearchWorkspaceSource& workspace,
                                   SearchCommandSource& commands) noexcept
    : workspace_{workspace}, commands_{commands} {}

void SearchController::openPalette(Revision revision) {
    state_.paletteOpen = true;
    state_.revision = revision;
    state_.query.clear();
    state_.mode = SearchMode::Command;
    rankPalette();
}

void SearchController::closePalette(Revision revision) {
    state_.paletteOpen = false;
    state_.revision = revision;
    state_.query.clear();
    state_.results.clear();
    state_.selectedIndex.reset();
}

void SearchController::updatePaletteQuery(std::string query,
                                            Revision revision) {
    if (!state_.paletteOpen) {
        return;
    }
    state_.query = std::move(query);
    state_.revision = revision;
    rankPalette();
}

void SearchController::rankPalette() {
    state_.mode = SearchMode::Command;
    state_.results.clear();
    for (const auto& command : commands_.descriptors()) {
        const auto labelScore = fuzzyScore(command.label, state_.query);
        const auto idScore = fuzzyScore(command.id, state_.query);
        if (!labelScore && !idScore) {
            continue;
        }
        state_.results.push_back(
            {.mode = SearchMode::Command,
             .path = command.id,
             .label = command.label,
             .score = std::max(labelScore.value_or(std::numeric_limits<int>::min()),
                               idScore.value_or(std::numeric_limits<int>::min()))});
    }
    std::ranges::sort(state_.results, [](const auto& left, const auto& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        if (left.label != right.label) {
            return left.label < right.label;
        }
        return left.path < right.path;  // Mirror palette_rank's stable id tiebreak.
    });
    state_.selectedIndex =
        state_.results.empty() ? std::nullopt : std::optional<std::size_t>{0};
}

void SearchController::selectNext() {
    if (!state_.selectedIndex || state_.results.empty()) {
        return;
    }
    *state_.selectedIndex = (*state_.selectedIndex + 1) % state_.results.size();
}

void SearchController::selectPrevious() {
    if (!state_.selectedIndex || state_.results.empty()) {
        return;
    }
    *state_.selectedIndex =
        (*state_.selectedIndex + state_.results.size() - 1) %
        state_.results.size();
}

PaletteExecutionResult SearchController::executePalette() {
    if (!state_.paletteOpen || !state_.selectedIndex ||
        *state_.selectedIndex >= state_.results.size()) {
        return {.accepted = false, .message = "no palette command selected"};
    }
    return commands_.execute(state_.results[*state_.selectedIndex].path);
}

WorkspaceSearchRequest SearchController::beginWorkspaceSearch(
    std::string query, Revision sourceRevision) {
    cancelWorkspaceSearch();
    WorkspaceSearchRequest request{
        .generation = state_.searchGeneration + 1,
        .sourceRevision = sourceRevision,
        .query = WorkspaceSearcher{}.parse(query)};
    state_.revision = sourceRevision;
    state_.query = std::move(query);
    state_.mode = request.query.mode;
    state_.results.clear();
    state_.selectedIndex.reset();
    state_.searchGeneration = request.generation;
    state_.searching = true;
    activeRequest_ = request;
    return request;
}

WorkspaceSearchBatch SearchController::evaluate(
    const WorkspaceSearchRequest& request) const {
    return WorkspaceSearcher{}.evaluate(workspace_, request);
}

void SearchController::cancelWorkspaceSearch() noexcept {
    if (activeRequest_) {
        activeRequest_->cancellation.cancel();
        state_.searching = false;
    }
}

SearchPublishResult SearchController::publish(
    const WorkspaceSearchBatch& batch, Revision currentRevision) {
    if (batch.cancelled) {
        return SearchPublishResult::Cancelled;
    }
    if (!activeRequest_ || batch.generation != activeRequest_->generation) {
        return SearchPublishResult::Superseded;
    }
    if (batch.sourceRevision != activeRequest_->sourceRevision ||
        batch.sourceRevision != currentRevision) {
        return SearchPublishResult::StaleRevision;
    }
    state_.results = batch.results;
    state_.selectedIndex =
        state_.results.empty() ? std::nullopt : std::optional<std::size_t>{0};
    state_.searching = false;
    activeRequest_.reset();
    return SearchPublishResult::Accepted;
}

} // namespace ssg
