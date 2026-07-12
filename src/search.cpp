#include <ssg/search.h>

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

std::optional<int> fuzzy_score(std::string_view candidate,
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

std::vector<SearchResult> rank_files(const WorkspaceSnapshot& workspace,
                                     std::string_view query,
                                     const SearchCancellationToken& token) {
    std::vector<SearchResult> results;
    for (const auto& file : workspace.files) {
        if (token.cancelled()) {
            return {};
        }
        const auto score = fuzzy_score(file.path, query);
        if (score) {
            results.push_back({.mode = SearchMode::file,
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

std::vector<SearchResult> rank_symbols(const WorkspaceSnapshot& workspace,
                                       std::string_view query,
                                       const SearchCancellationToken& token) {
    std::vector<SearchResult> results;
    for (const auto& symbol : workspace.symbols) {
        if (token.cancelled()) {
            return {};
        }
        const auto score = fuzzy_score(symbol.name, query);
        if (score) {
            results.push_back(
                {.mode = SearchMode::symbol,
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
        const auto left_name = left.label.substr(left.label.find(':') + 1);
        const auto right_name = right.label.substr(right.label.find(':') + 1);
        if (left_name != right_name) {
            return left_name < right_name;
        }
        return left.path < right.path;
    });
    return results;
}

std::vector<SearchResult> rank_text(const WorkspaceSnapshot& workspace,
                                    std::string_view query,
                                    const SearchCancellationToken& token) {
    std::vector<SearchResult> results;
    if (query.empty()) {
        return results;
    }
    for (const auto& file : workspace.files) {
        std::size_t line_start = 0;
        std::size_t line_number = 0;
        while (line_start <= file.text.size()) {
            if (token.cancelled()) {
                return {};
            }
            const auto line_end = file.text.find('\n', line_start);
            const auto line = std::string_view{file.text}.substr(
                line_start, line_end == std::string::npos
                                ? file.text.size() - line_start
                                : line_end - line_start);
            const auto match = line.find(query);
            if (match != std::string_view::npos) {
                results.push_back(
                    {.mode = SearchMode::text,
                     .path = file.path,
                     .label = file.path + ":" + std::to_string(line_number + 1),
                     .line = LineIndex{line_number},
                     .column = match + 1,
                     .score = 0});
            }
            if (line_end == std::string::npos) {
                break;
            }
            line_start = line_end + 1;
            ++line_number;
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

NavigationTransition empty_transition() { return {}; }

} // namespace

ParsedSearchQuery parse_search_query(std::string_view query) {
    ParsedSearchQuery result;
    if (query.empty()) {
        return result;
    }
    switch (query.front()) {
    case '@':
        result.mode = SearchMode::symbol;
        result.text = query.substr(1);
        return result;
    case '#':
        result.mode = SearchMode::text;
        result.text = query.substr(1);
        return result;
    case ':': {
        result.mode = SearchMode::line;
        result.text = query.substr(1);
        std::size_t one_based = 0;
        const auto [end, error] = std::from_chars(
            result.text.data(), result.text.data() + result.text.size(), one_based);
        if (result.text.empty() || error != std::errc{} ||
            end != result.text.data() + result.text.size() || one_based == 0) {
            result.error = SearchQueryError::invalid_line;
            return result;
        }
        result.line = LineIndex{one_based - 1};
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

std::vector<SearchResult> rank_workspace(
    const WorkspaceSnapshot& workspace, const ParsedSearchQuery& query,
    const SearchCancellationToken& cancellation) {
    if (query.error != SearchQueryError::none || cancellation.cancelled()) {
        return {};
    }
    switch (query.mode) {
    case SearchMode::file:
        return rank_files(workspace, query.text, cancellation);
    case SearchMode::symbol:
        return rank_symbols(workspace, query.text, cancellation);
    case SearchMode::text:
        return rank_text(workspace, query.text, cancellation);
    case SearchMode::line:
    case SearchMode::command:
        return {};
    }
    return {};
}

WorkspaceSearchBatch evaluate_workspace_search(
    const SearchWorkspaceSource& source,
    const WorkspaceSearchRequest& request) {
    WorkspaceSearchBatch batch{.generation = request.generation,
                               .source_revision = request.source_revision};
    if (request.cancellation.cancelled()) {
        batch.cancelled = true;
        return batch;
    }
    const auto workspace = source.snapshot(request.source_revision);
    batch.source_revision = workspace.revision;
    batch.results =
        rank_workspace(workspace, request.query, request.cancellation);
    batch.cancelled = request.cancellation.cancelled();
    return batch;
}

std::optional<NavigationTarget> navigation_target(const SearchResult& result) {
    if (result.path.empty()) {
        return std::nullopt;
    }
    return NavigationTarget{.path = result.path,
                            .line = result.line.value_or(LineIndex{0}),
                            .column = result.column,
                            .symbol = result.mode == SearchMode::symbol
                                          ? std::optional<std::string>{
                                                result.label.substr(
                                                    result.label.find(':') + 1)}
                                          : std::nullopt};
}

std::optional<NavigationTarget> goto_line(
    std::string path, const ParsedSearchQuery& query) {
    if (path.empty() || query.mode != SearchMode::line ||
        query.error != SearchQueryError::none || !query.line) {
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
            .pause_follow_edits = origin == NavigationOrigin::user,
            .reveal_primary_caret = true};
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
        return empty_transition();
    }
    --*cursor_;
    return transition(entries_[*cursor_], NavigationOrigin::user);
}

NavigationTransition NavigationHistory::forward() {
    if (!cursor_ || *cursor_ + 1 >= entries_.size()) {
        return empty_transition();
    }
    ++*cursor_;
    return transition(entries_[*cursor_], NavigationOrigin::user);
}

SearchController::SearchController(const SearchWorkspaceSource& workspace,
                                   SearchCommandSource& commands) noexcept
    : workspace_{workspace}, commands_{commands} {}

void SearchController::open_palette(Revision revision) {
    state_.palette_open = true;
    state_.revision = revision;
    state_.query.clear();
    state_.mode = SearchMode::command;
    rank_palette();
}

void SearchController::close_palette(Revision revision) {
    state_.palette_open = false;
    state_.revision = revision;
    state_.query.clear();
    state_.results.clear();
    state_.selected_index.reset();
}

void SearchController::update_palette_query(std::string query,
                                            Revision revision) {
    if (!state_.palette_open) {
        return;
    }
    state_.query = std::move(query);
    state_.revision = revision;
    rank_palette();
}

void SearchController::rank_palette() {
    state_.mode = SearchMode::command;
    state_.results.clear();
    for (const auto& command : commands_.descriptors()) {
        const auto label_score = fuzzy_score(command.label, state_.query);
        const auto id_score = fuzzy_score(command.id, state_.query);
        if (!label_score && !id_score) {
            continue;
        }
        state_.results.push_back(
            {.mode = SearchMode::command,
             .path = command.id,
             .label = command.label,
             .score = std::max(label_score.value_or(std::numeric_limits<int>::min()),
                               id_score.value_or(std::numeric_limits<int>::min()))});
    }
    std::ranges::sort(state_.results, [](const auto& left, const auto& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.label < right.label;
    });
    state_.selected_index =
        state_.results.empty() ? std::nullopt : std::optional<std::size_t>{0};
}

void SearchController::select_next() {
    if (!state_.selected_index || state_.results.empty()) {
        return;
    }
    *state_.selected_index = (*state_.selected_index + 1) % state_.results.size();
}

void SearchController::select_previous() {
    if (!state_.selected_index || state_.results.empty()) {
        return;
    }
    *state_.selected_index =
        (*state_.selected_index + state_.results.size() - 1) %
        state_.results.size();
}

PaletteExecutionResult SearchController::execute_palette() {
    if (!state_.palette_open || !state_.selected_index ||
        *state_.selected_index >= state_.results.size()) {
        return {.accepted = false, .message = "no palette command selected"};
    }
    return commands_.execute(state_.results[*state_.selected_index].path);
}

WorkspaceSearchRequest SearchController::begin_workspace_search(
    std::string query, Revision source_revision) {
    cancel_workspace_search();
    WorkspaceSearchRequest request{
        .generation = state_.search_generation + 1,
        .source_revision = source_revision,
        .query = parse_search_query(query)};
    state_.revision = source_revision;
    state_.query = std::move(query);
    state_.mode = request.query.mode;
    state_.results.clear();
    state_.selected_index.reset();
    state_.search_generation = request.generation;
    state_.searching = true;
    active_request_ = request;
    return request;
}

WorkspaceSearchBatch SearchController::evaluate(
    const WorkspaceSearchRequest& request) const {
    return evaluate_workspace_search(workspace_, request);
}

void SearchController::cancel_workspace_search() noexcept {
    if (active_request_) {
        active_request_->cancellation.cancel();
        state_.searching = false;
    }
}

SearchPublishResult SearchController::publish(
    const WorkspaceSearchBatch& batch, Revision current_revision) {
    if (batch.cancelled) {
        return SearchPublishResult::cancelled;
    }
    if (!active_request_ || batch.generation != active_request_->generation) {
        return SearchPublishResult::superseded;
    }
    if (batch.source_revision != active_request_->source_revision ||
        batch.source_revision != current_revision) {
        return SearchPublishResult::stale_revision;
    }
    state_.results = batch.results;
    state_.selected_index =
        state_.results.empty() ? std::nullopt : std::optional<std::size_t>{0};
    state_.searching = false;
    active_request_.reset();
    return SearchPublishResult::accepted;
}

SearchCommandSet search_command_set() { return {}; }

SearchDelta derive_search_delta(const SearchViewState& base,
                                const SearchViewState& target) {
    return {.base_revision = base.revision,
            .revision = target.revision,
            .state = base == target ? std::nullopt
                                    : std::optional<SearchViewState>{target}};
}

SearchReplayResult replay_search_delta(const SearchViewState& base,
                                       const SearchDelta& delta) {
    if (base.revision != delta.base_revision) {
        return {.error = SearchReplayError::stale_revision};
    }
    if (!delta.state) {
        if (delta.revision == base.revision) {
            return {.state = base};
        }
        return {.error = SearchReplayError::malformed_delta};
    }
    if (delta.state->revision != delta.revision) {
        return {.error = SearchReplayError::malformed_delta};
    }
    return {.state = delta.state};
}

} // namespace ssg
