#include <ssg/find_replace.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

namespace ssg {
namespace {

struct ScalarText {
    std::vector<char32_t> values;
    std::vector<std::size_t> bytes;
};

bool decodeUtf8(std::string_view text, ScalarText& out) {
    out.values.clear();
    out.bytes.clear();
    std::size_t at = 0;
    while (at < text.size()) {
        out.bytes.push_back(at);
        const auto first = static_cast<unsigned char>(text[at]);
        std::size_t length = 0;
        char32_t value = 0;
        if (first < 0x80U) {
            length = 1;
            value = first;
        } else if ((first & 0xE0U) == 0xC0U) {
            length = 2;
            value = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            length = 3;
            value = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            length = 4;
            value = first & 0x07U;
        } else {
            return false;
        }
        if (at + length > text.size()) {
            return false;
        }
        for (std::size_t index = 1; index < length; ++index) {
            const auto continuation =
                static_cast<unsigned char>(text[at + index]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            value = (value << 6U) | (continuation & 0x3FU);
        }
        if ((length == 2 && value < 0x80U) ||
            (length == 3 && value < 0x800U) ||
            (length == 4 && value < 0x10000U) ||
            (value >= 0xD800U && value <= 0xDFFFU) || value > 0x10FFFFU) {
            return false;
        }
        out.values.push_back(value);
        at += length;
    }
    out.bytes.push_back(text.size());
    return true;
}

char32_t folded(char32_t value, bool case_sensitive) {
    if (!case_sensitive && value >= U'A' && value <= U'Z') {
        return value + (U'a' - U'A');
    }
    return value;
}

bool wordScalar(char32_t value) {
    return value >= 0x80U || value == U'_' ||
           (value >= U'0' && value <= U'9') ||
           (value >= U'a' && value <= U'z') ||
           (value >= U'A' && value <= U'Z');
}

std::optional<std::size_t> scalarIndex(const ScalarText& text,
                                        std::uint64_t byte) {
    const auto it = std::lower_bound(text.bytes.begin(), text.bytes.end(), byte);
    if (it == text.bytes.end() || *it != byte) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - text.bytes.begin());
}

enum class NodeKind {
    Empty,
    Literal,
    Any,
    CharacterClass,
    Begin,
    End,
    Concat,
    Alternate,
    Repeat,
};

struct CharacterRange {
    char32_t first;
    char32_t last;
};

struct Node {
    NodeKind kind = NodeKind::Empty;
    char32_t literal = 0;
    bool negated = false;
    std::vector<CharacterRange> ranges;
    std::vector<std::shared_ptr<Node>> children;
    std::size_t minimum = 0;
    std::optional<std::size_t> maximum;
};

class RegexParser {
public:
    explicit RegexParser(std::vector<char32_t> pattern)
        : pattern_(std::move(pattern)) {}

    std::shared_ptr<Node> parse() {
        auto result = expression();
        if (!valid_ || at_ != pattern_.size()) {
            return {};
        }
        return result;
    }

private:
    std::shared_ptr<Node> expression() {
        std::vector<std::shared_ptr<Node>> alternatives;
        alternatives.push_back(sequence());
        while (take(U'|')) {
            alternatives.push_back(sequence());
        }
        if (alternatives.size() == 1) {
            return alternatives.front();
        }
        auto node = std::make_shared<Node>();
        node->kind = NodeKind::Alternate;
        node->children = std::move(alternatives);
        return node;
    }

    std::shared_ptr<Node> sequence() {
        std::vector<std::shared_ptr<Node>> parts;
        while (at_ < pattern_.size() && pattern_[at_] != U')' &&
               pattern_[at_] != U'|') {
            parts.push_back(factor());
        }
        if (parts.empty()) {
            return std::make_shared<Node>();
        }
        if (parts.size() == 1) {
            return parts.front();
        }
        auto node = std::make_shared<Node>();
        node->kind = NodeKind::Concat;
        node->children = std::move(parts);
        return node;
    }

    std::shared_ptr<Node> factor() {
        auto value = atom();
        if (!value || at_ >= pattern_.size()) {
            return value;
        }
        std::size_t minimum = 0;
        std::optional<std::size_t> maximum;
        if (take(U'*')) {
            minimum = 0;
        } else if (take(U'+')) {
            minimum = 1;
        } else if (take(U'?')) {
            minimum = 0;
            maximum = 1;
        } else {
            return value;
        }
        auto node = std::make_shared<Node>();
        node->kind = NodeKind::Repeat;
        node->children.push_back(std::move(value));
        node->minimum = minimum;
        node->maximum = maximum;
        return node;
    }

    std::shared_ptr<Node> atom() {
        if (at_ >= pattern_.size()) {
            valid_ = false;
            return {};
        }
        if (take(U'(')) {
            if (++depth_ > 256) {
                valid_ = false;
                return {};
            }
            auto value = expression();
            if (!take(U')')) {
                valid_ = false;
            }
            --depth_;
            return value;
        }
        if (take(U'.')) {
            auto node = std::make_shared<Node>();
            node->kind = NodeKind::Any;
            return node;
        }
        if (take(U'^')) {
            auto node = std::make_shared<Node>();
            node->kind = NodeKind::Begin;
            return node;
        }
        if (take(U'$')) {
            auto node = std::make_shared<Node>();
            node->kind = NodeKind::End;
            return node;
        }
        if (take(U'[')) {
            return characterClass();
        }
        char32_t value = pattern_[at_++];
        if (value == U'\\') {
            if (at_ == pattern_.size()) {
                valid_ = false;
                return {};
            }
            value = pattern_[at_++];
        } else if (value == U'*' || value == U'+' || value == U'?' ||
                   value == U')') {
            valid_ = false;
            return {};
        }
        auto node = std::make_shared<Node>();
        node->kind = NodeKind::Literal;
        node->literal = value;
        return node;
    }

    std::shared_ptr<Node> characterClass() {
        auto node = std::make_shared<Node>();
        node->kind = NodeKind::CharacterClass;
        node->negated = take(U'^');
        bool any = false;
        while (at_ < pattern_.size() && pattern_[at_] != U']') {
            char32_t first = classScalar();
            char32_t last = first;
            if (at_ + 1 < pattern_.size() && pattern_[at_] == U'-' &&
                pattern_[at_ + 1] != U']') {
                ++at_;
                last = classScalar();
                if (last < first) {
                    valid_ = false;
                }
            }
            node->ranges.push_back({first, last});
            any = true;
        }
        if (!any || !take(U']')) {
            valid_ = false;
        }
        return node;
    }

    char32_t classScalar() {
        if (at_ >= pattern_.size()) {
            valid_ = false;
            return 0;
        }
        char32_t value = pattern_[at_++];
        if (value == U'\\') {
            if (at_ >= pattern_.size()) {
                valid_ = false;
                return 0;
            }
            value = pattern_[at_++];
        }
        return value;
    }

    bool take(char32_t value) {
        if (at_ < pattern_.size() && pattern_[at_] == value) {
            ++at_;
            return true;
        }
        return false;
    }

    std::vector<char32_t> pattern_;
    std::size_t at_ = 0;
    std::size_t depth_ = 0;
    bool valid_ = true;
};

struct MatchContext {
    const ScalarText& text;
    bool case_sensitive;
    std::size_t lower;
    std::size_t upper;
    std::uint64_t remaining;
    const std::atomic_bool* cancelled;
    FindReplaceError error = FindReplaceError::None;

    bool step() {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            error = FindReplaceError::Cancelled;
            return false;
        }
        if (remaining == 0) {
            error = FindReplaceError::BudgetExhausted;
            return false;
        }
        --remaining;
        return true;
    }
};

void unique(std::vector<std::size_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::vector<std::size_t> evaluateNode(const std::shared_ptr<Node>& node,
                                       std::size_t position,
                                       MatchContext& context);

std::vector<std::size_t> evaluateRepeat(const Node& node,
                                         std::size_t position,
                                         MatchContext& context) {
    std::vector<std::size_t> frontier{position};
    std::vector<std::size_t> accepted;
    std::vector<std::size_t> seen{position};
    std::size_t count = 0;
    if (node.minimum == 0) {
        accepted.push_back(position);
    }
    while (!frontier.empty() &&
           (!node.maximum || count < *node.maximum)) {
        std::vector<std::size_t> next;
        for (const auto at : frontier) {
            auto ends = evaluateNode(node.children.front(), at, context);
            if (context.error != FindReplaceError::None) {
                return {};
            }
            next.insert(next.end(), ends.begin(), ends.end());
        }
        unique(next);
        ++count;
        if (count >= node.minimum) {
            accepted.insert(accepted.end(), next.begin(), next.end());
        }
        std::vector<std::size_t> fresh;
        for (const auto value : next) {
            if (std::find(seen.begin(), seen.end(), value) == seen.end()) {
                seen.push_back(value);
                fresh.push_back(value);
            }
        }
        frontier = std::move(fresh);
    }
    unique(accepted);
    return accepted;
}

std::vector<std::size_t> evaluateNode(const std::shared_ptr<Node>& node,
                                       std::size_t position,
                                       MatchContext& context) {
    if (!context.step()) {
        return {};
    }
    switch (node->kind) {
    case NodeKind::Empty:
        return {position};
    case NodeKind::Literal:
        if (position < context.upper &&
            folded(context.text.values[position], context.case_sensitive) ==
                folded(node->literal, context.case_sensitive)) {
            return {position + 1};
        }
        return {};
    case NodeKind::Any:
        return position < context.upper
                   ? std::vector<std::size_t>{position + 1}
                   : std::vector<std::size_t>{};
    case NodeKind::CharacterClass: {
        if (position >= context.upper) {
            return {};
        }
        const auto value =
            folded(context.text.values[position], context.case_sensitive);
        bool inside = false;
        for (const auto range : node->ranges) {
            inside = inside ||
                     (value >= folded(range.first, context.case_sensitive) &&
                      value <= folded(range.last, context.case_sensitive));
        }
        return inside != node->negated
                   ? std::vector<std::size_t>{position + 1}
                   : std::vector<std::size_t>{};
    }
    case NodeKind::Begin:
        return position == context.lower
                   ? std::vector<std::size_t>{position}
                   : std::vector<std::size_t>{};
    case NodeKind::End:
        return position == context.upper
                   ? std::vector<std::size_t>{position}
                   : std::vector<std::size_t>{};
    case NodeKind::Alternate: {
        std::vector<std::size_t> result;
        for (const auto& child : node->children) {
            auto values = evaluateNode(child, position, context);
            result.insert(result.end(), values.begin(), values.end());
            if (context.error != FindReplaceError::None) {
                return {};
            }
        }
        unique(result);
        return result;
    }
    case NodeKind::Concat: {
        std::vector<std::size_t> positions{position};
        for (const auto& child : node->children) {
            std::vector<std::size_t> next;
            for (const auto at : positions) {
                auto values = evaluateNode(child, at, context);
                next.insert(next.end(), values.begin(), values.end());
                if (context.error != FindReplaceError::None) {
                    return {};
                }
            }
            unique(next);
            positions = std::move(next);
            if (positions.empty()) {
                break;
            }
        }
        return positions;
    }
    case NodeKind::Repeat:
        return evaluateRepeat(*node, position, context);
    }
    return {};
}

bool wholeWordMatch(const ScalarText& text, std::size_t begin,
                      std::size_t end, std::size_t lower,
                      std::size_t upper) {
    return (begin == lower || !wordScalar(text.values[begin - 1])) &&
           (end == upper || !wordScalar(text.values[end]));
}

FindResult literalMatches(const ScalarText& text, const ScalarText& query,
                           const FindRequest& request, std::size_t lower,
                           std::size_t upper) {
    FindResult result;
    std::uint64_t remaining = request.work_budget;
    if (query.values.empty()) {
        return result;
    }
    std::size_t at = lower;
    while (at + query.values.size() <= upper) {
        if (request.cancelled &&
            request.cancelled->load(std::memory_order_relaxed)) {
            return {FindReplaceError::Cancelled, {}, "find was cancelled"};
        }
        bool matches = true;
        for (std::size_t index = 0; index < query.values.size(); ++index) {
            if (remaining == 0) {
                return {FindReplaceError::BudgetExhausted, {},
                        "find work budget exhausted"};
            }
            --remaining;
            matches =
                matches &&
                folded(text.values[at + index],
                       request.options.case_sensitive) ==
                    folded(query.values[index],
                           request.options.case_sensitive);
        }
        if (matches &&
            (!request.options.whole_word ||
             wholeWordMatch(text, at, at + query.values.size(), lower,
                              upper))) {
            result.matches.push_back(
                {ByteOffset{text.bytes[at]},
                 ByteOffset{text.bytes[at + query.values.size()]}});
            at += query.values.size();
        } else {
            ++at;
        }
    }
    return result;
}

FindReplaceOperationResult operationFailure(FindReplaceError error,
                                             Revision revision,
                                             std::string message) {
    return {error, revision, std::move(message)};
}

std::string replacedText(std::string_view original,
                          const std::vector<FindMatch>& matches,
                          std::string_view replacement) {
    std::string result;
    std::size_t copied = 0;
    for (const auto& match : matches) {
        const auto begin = static_cast<std::size_t>(match.begin.value());
        const auto end = static_cast<std::size_t>(match.end.value());
        result.append(original.substr(copied, begin - copied));
        result.append(replacement);
        copied = end;
    }
    result.append(original.substr(copied));
    return result;
}

}  // namespace

FindResult findMatches(std::string_view text, const FindRequest& request) {
    ScalarText decoded_text;
    ScalarText decoded_query;
    if (!decodeUtf8(text, decoded_text) ||
        !decodeUtf8(request.query, decoded_query)) {
        return {FindReplaceError::InvalidUtf8, {}, "input is not valid UTF-8"};
    }
    std::size_t lower = 0;
    std::size_t upper = decoded_text.values.size();
    if (request.options.selection_only) {
        if (!request.selection ||
            request.selection->begin > request.selection->end) {
            return {FindReplaceError::InvalidSelection, {},
                    "selection-limited find requires an ordered selection"};
        }
        const auto begin =
            scalarIndex(decoded_text, request.selection->begin.value());
        const auto end =
            scalarIndex(decoded_text, request.selection->end.value());
        if (!begin || !end) {
            return {FindReplaceError::InvalidSelection, {},
                    "selection must use UTF-8 boundaries"};
        }
        lower = *begin;
        upper = *end;
    }
    if (!request.options.regex) {
        return literalMatches(decoded_text, decoded_query, request, lower,
                               upper);
    }
    if (decoded_query.values.empty()) {
        return {};
    }
    if (decoded_query.values.size() > 4096) {
        return {FindReplaceError::InvalidPattern, {},
                "regex pattern exceeds the complexity limit"};
    }
    if (decoded_query.values.size() > request.work_budget) {
        return {FindReplaceError::BudgetExhausted, {},
                "regex work budget exhausted while parsing"};
    }

    RegexParser parser{decoded_query.values};
    const auto root = parser.parse();
    if (!root) {
        return {FindReplaceError::InvalidPattern, {},
                "regex pattern is invalid"};
    }
    MatchContext context{decoded_text,
                         request.options.case_sensitive,
                         lower,
                         upper,
                         request.work_budget - decoded_query.values.size(),
                         request.cancelled};
    FindResult result;
    std::size_t at = lower;
    while (at <= upper) {
        auto ends = evaluateNode(root, at, context);
        if (context.error != FindReplaceError::None) {
            return {context.error, {},
                    context.error == FindReplaceError::Cancelled
                        ? "find was cancelled"
                        : "regex work budget exhausted"};
        }
        if (!ends.empty()) {
            if (request.options.whole_word) {
                std::erase_if(ends, [&](std::size_t end) {
                    return !wholeWordMatch(decoded_text, at, end, lower,
                                             upper);
                });
            }
            if (!ends.empty()) {
                const auto end = *std::max_element(ends.begin(), ends.end());
                result.matches.push_back(
                    {ByteOffset{decoded_text.bytes[at]},
                     ByteOffset{decoded_text.bytes[end]}});
                if (end > at) {
                    at = end;
                    continue;
                }
            }
        }
        ++at;
    }
    return result;
}

FindReplaceCommandSet::FindReplaceCommandSet()
    : descriptors_{{{"find.open", FindReplaceCommand::FindOpen},
                    {"find.close", FindReplaceCommand::FindClose},
                    {"find.next", FindReplaceCommand::FindNext},
                    {"find.previous", FindReplaceCommand::FindPrevious},
                    {"find.update_query",
                     FindReplaceCommand::FindUpdateQuery},
                    {"find.toggle_case",
                     FindReplaceCommand::FindToggleCase},
                    {"find.toggle_whole_word",
                     FindReplaceCommand::FindToggleWholeWord},
                    {"find.toggle_regex",
                     FindReplaceCommand::FindToggleRegex},
                    {"find.toggle_selection",
                     FindReplaceCommand::FindToggleSelection},
                    {"replace.open", FindReplaceCommand::ReplaceOpen},
                    {"replace.update_replacement",
                     FindReplaceCommand::ReplaceUpdateReplacement},
                    {"replace.current",
                     FindReplaceCommand::ReplaceCurrent},
                    {"replace.all", FindReplaceCommand::ReplaceAll},
                    {"replace.workspace_preview",
                     FindReplaceCommand::ReplaceWorkspacePreview},
                    {"replace.workspace_apply",
                     FindReplaceCommand::ReplaceWorkspaceApply}}} {}

const std::array<FindReplaceCommandDescriptor, 15>&
FindReplaceCommandSet::descriptors() const noexcept {
    return descriptors_;
}

FindReplaceCommandSet findReplaceCommandSet() {
    return FindReplaceCommandSet{};
}

FindReplaceDelta deriveFindReplaceDelta(
    const FindReplaceViewState& before, const FindReplaceViewState& after) {
    if (before == after) {
        return {false, before.generation, std::nullopt};
    }
    return {true, before.generation, after};
}

FindReplaceReplayResult replayFindReplaceDelta(
    const FindReplaceViewState& base, const FindReplaceDelta& delta) {
    if (base.generation != delta.base_generation) {
        return {FindReplaceReplayError::BaseMismatch, base};
    }
    if (delta.changed != delta.replacement.has_value()) {
        return {FindReplaceReplayError::MalformedDelta, base};
    }
    return {FindReplaceReplayError::None,
            delta.replacement ? *delta.replacement : base};
}

void FindReplaceController::open(const DocumentSnapshot& document,
                                 FindRequest request) {
    request_ = std::move(request);
    state_.open = true;
    state_.replace_mode = false;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::openReplace(const DocumentSnapshot& document,
                                         FindRequest request) {
    request_ = std::move(request);
    state_.open = true;
    state_.replace_mode = true;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::close() {
    state_.open = false;
    state_.replace_mode = false;
    state_.replacement.clear();
    state_.matches.clear();
    state_.active_match.reset();
    state_.error = FindReplaceError::None;
    state_.message.clear();
    ++state_.generation;
}

void FindReplaceController::updateQuery(
    const DocumentSnapshot& document, std::string query,
    std::optional<ByteRange> selection) {
    request_.query = std::move(query);
    request_.selection = selection;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::updateReplacement(std::string replacement) {
    // The replacement does not affect matching, so this never re-evaluates; it
    // only updates the published replacement text (the single source of truth
    // that replace_current/replace_all read).
    state_.replacement = std::move(replacement);
    ++state_.generation;
}

void FindReplaceController::toggleCase(const DocumentSnapshot& document) {
    request_.options.case_sensitive = !request_.options.case_sensitive;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::toggleWholeWord(
    const DocumentSnapshot& document) {
    request_.options.whole_word = !request_.options.whole_word;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::toggleRegex(const DocumentSnapshot& document) {
    request_.options.regex = !request_.options.regex;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::toggleSelection(
    const DocumentSnapshot& document, std::optional<ByteRange> selection) {
    request_.options.selection_only = !request_.options.selection_only;
    request_.selection = selection;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::refresh(const DocumentSnapshot& document,
                                    std::optional<ByteRange> selection) {
    request_.selection = selection;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::evaluate(const DocumentSnapshot& document) {
    state_.source_revision = document.revision;
    state_.query = request_.query;
    state_.options = request_.options;
    auto result = findMatches(document.text, request_);
    state_.matches = std::move(result.matches);
    state_.error = result.error;
    state_.message = std::move(result.message);
    state_.active_match =
        state_.matches.empty() ? std::nullopt : std::optional<std::size_t>{0};
}

void FindReplaceController::next() {
    if (!state_.matches.empty()) {
        state_.active_match =
            (state_.active_match.value_or(0) + 1) % state_.matches.size();
        ++state_.generation;
    }
}

void FindReplaceController::previous() {
    if (!state_.matches.empty()) {
        const auto current = state_.active_match.value_or(0);
        state_.active_match =
            (current + state_.matches.size() - 1) % state_.matches.size();
        ++state_.generation;
    }
}

FindReplaceOperationResult FindReplaceController::replaceCurrent(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections_before,
    const SelectionSet& selections_after, std::string replacement,
    std::uint64_t timestamp_ms) {
    if (document.revision() != state_.source_revision) {
        return operationFailure(FindReplaceError::StaleRevision,
                                 document.revision(),
                                 "find result revision is stale");
    }
    if (!state_.active_match || state_.error != FindReplaceError::None) {
        return operationFailure(FindReplaceError::NoMatch,
                                 document.revision(), "no active match");
    }
    const auto match = state_.matches[*state_.active_match];
    if (match.begin == match.end && replacement.empty()) {
        return operationFailure(FindReplaceError::DocumentRejected,
                                 document.revision(),
                                 "replacement would not change the document");
    }
    EditTransaction transaction{
        state_.source_revision,
        {{match.begin, match.end.value() - match.begin.value(),
          std::move(replacement)}}};
    auto history_result = history.applyEdit(
        document, transaction, selections_before, selections_after,
        HistoryEditKind::Other, timestamp_ms);
    if (!history_result.accepted()) {
        return operationFailure(
            history_result.error == HistoryError::StaleDocument
                ? FindReplaceError::StaleRevision
                : FindReplaceError::DocumentRejected,
            history_result.revision, history_result.message);
    }
    ++state_.generation;
    evaluate(document.snapshot());
    return {FindReplaceError::None, document.revision(), {}};
}

FindReplaceOperationResult FindReplaceController::replaceAll(
    Document& document, DocumentHistory& history,
    const SelectionSet& selections_before,
    const SelectionSet& selections_after, std::string replacement,
    std::uint64_t timestamp_ms) {
    if (document.revision() != state_.source_revision) {
        return operationFailure(FindReplaceError::StaleRevision,
                                 document.revision(),
                                 "find result revision is stale");
    }
    if (state_.matches.empty() || state_.error != FindReplaceError::None) {
        return operationFailure(FindReplaceError::NoMatch,
                                 document.revision(), "no matches");
    }
    EditTransaction transaction{state_.source_revision, {}};
    transaction.edits.reserve(state_.matches.size());
    for (const auto& match : state_.matches) {
        if (match.begin == match.end && replacement.empty()) {
            continue;
        }
        transaction.edits.push_back(
            {match.begin, match.end.value() - match.begin.value(),
             replacement});
    }
    if (transaction.edits.empty()) {
        return operationFailure(FindReplaceError::DocumentRejected,
                                 document.revision(),
                                 "replacement would not change the document");
    }
    auto history_result = history.applyEdit(
        document, transaction, selections_before, selections_after,
        HistoryEditKind::Other, timestamp_ms);
    if (!history_result.accepted()) {
        return operationFailure(
            history_result.error == HistoryError::StaleDocument
                ? FindReplaceError::StaleRevision
                : FindReplaceError::DocumentRejected,
            history_result.revision, history_result.message);
    }
    ++state_.generation;
    evaluate(document.snapshot());
    return {FindReplaceError::None, document.revision(), {}};
}

const FindReplaceViewState& FindReplaceController::viewState() const noexcept {
    return state_;
}

WorkspacePreviewResult previewWorkspaceReplace(
    const FindReplaceWorkspace& workspace, Revision source_revision,
    const FindRequest& request, std::string replacement) {
    if (request.options.selection_only) {
        return {FindReplaceError::InvalidSelection, std::nullopt,
                "workspace replace cannot use a document selection"};
    }
    const auto snapshot = workspace.snapshot(source_revision);
    if (snapshot.revision != source_revision) {
        return {FindReplaceError::StaleRevision, std::nullopt,
                "workspace snapshot revision is stale"};
    }
    WorkspaceReplacePreview preview{source_revision,
                                    request.query,
                                    std::move(replacement),
                                    request.options,
                                    {}};
    const auto per_file_budget =
        snapshot.files.empty()
            ? request.work_budget
            : request.work_budget /
                  static_cast<std::uint64_t>(snapshot.files.size());
    for (const auto& file : snapshot.files) {
        auto file_request = request;
        file_request.work_budget = per_file_budget;
        auto result = findMatches(file.text, file_request);
        if (!result.accepted()) {
            return {result.error, std::nullopt, std::move(result.message)};
        }
        if (!result.matches.empty()) {
            preview.changes.push_back(
                {file.path, file.text,
                 replacedText(file.text, result.matches,
                               preview.replacement),
                 std::move(result.matches)});
        }
    }
    return {FindReplaceError::None, std::move(preview), {}};
}

WorkspaceApplyResult applyWorkspaceReplace(
    FindReplaceWorkspace& workspace, const WorkspaceReplacePreview& preview,
    WorkspaceRecoverySink& recovery_sink) {
    return workspace.apply(preview, recovery_sink);
}

WorkspaceApplyResult recoverWorkspaceReplace(
    FindReplaceWorkspace& workspace, const WorkspaceRecoveryRecord& record) {
    return workspace.recover(record);
}

}  // namespace ssg
