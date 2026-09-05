#include <ssg/FindReplace.h>

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

char32_t folded(char32_t value, bool caseSensitive) {
    if (!caseSensitive && value >= U'A' && value <= U'Z') {
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
    bool caseSensitive;
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
            folded(context.text.values[position], context.caseSensitive) ==
                folded(node->literal, context.caseSensitive)) {
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
            folded(context.text.values[position], context.caseSensitive);
        bool inside = false;
        for (const auto range : node->ranges) {
            inside = inside ||
                     (value >= folded(range.first, context.caseSensitive) &&
                      value <= folded(range.last, context.caseSensitive));
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
    std::uint64_t remaining = request.workBudget;
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
                       request.options.caseSensitive) ==
                    folded(query.values[index],
                           request.options.caseSensitive);
        }
        if (matches &&
            (!request.options.wholeWord ||
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
                                             std::uint64_t revision,
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

FindResult findTextMatches(std::string_view text, const FindRequest& request) {
    ScalarText decodedText;
    ScalarText decodedQuery;
    if (!decodeUtf8(text, decodedText) ||
        !decodeUtf8(request.query, decodedQuery)) {
        return {FindReplaceError::InvalidUtf8, {}, "input is not valid UTF-8"};
    }
    std::size_t lower = 0;
    std::size_t upper = decodedText.values.size();
    if (request.options.selectionOnly) {
        if (!request.selection ||
            request.selection->begin > request.selection->end) {
            return {FindReplaceError::InvalidSelection, {},
                    "selection-limited find requires an ordered selection"};
        }
        const auto begin =
            scalarIndex(decodedText, request.selection->begin.value());
        const auto end =
            scalarIndex(decodedText, request.selection->end.value());
        if (!begin || !end) {
            return {FindReplaceError::InvalidSelection, {},
                    "selection must use UTF-8 boundaries"};
        }
        lower = *begin;
        upper = *end;
    }
    if (!request.options.regex) {
        return literalMatches(decodedText, decodedQuery, request, lower,
                               upper);
    }
    if (decodedQuery.values.empty()) {
        return {};
    }
    if (decodedQuery.values.size() > 4096) {
        return {FindReplaceError::InvalidPattern, {},
                "regex pattern exceeds the complexity limit"};
    }
    if (decodedQuery.values.size() > request.workBudget) {
        return {FindReplaceError::BudgetExhausted, {},
                "regex work budget exhausted while parsing"};
    }

    RegexParser parser{decodedQuery.values};
    const auto root = parser.parse();
    if (!root) {
        return {FindReplaceError::InvalidPattern, {},
                "regex pattern is invalid"};
    }
    MatchContext context{decodedText,
                         request.options.caseSensitive,
                         lower,
                         upper,
                         request.workBudget - decodedQuery.values.size(),
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
            if (request.options.wholeWord) {
                std::erase_if(ends, [&](std::size_t end) {
                    return !wholeWordMatch(decodedText, at, end, lower,
                                             upper);
                });
            }
            if (!ends.empty()) {
                const auto end = *std::max_element(ends.begin(), ends.end());
                result.matches.push_back(
                    {ByteOffset{decodedText.bytes[at]},
                     ByteOffset{decodedText.bytes[end]}});
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

void FindReplaceController::open(const DocumentSnapshot& document,
                                 FindRequest request) {
    request_ = std::move(request);
    state_.open = true;
    state_.replaceMode = false;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::openReplace(const DocumentSnapshot& document,
                                         FindRequest request) {
    request_ = std::move(request);
    state_.open = true;
    state_.replaceMode = true;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::close() {
    state_.open = false;
    state_.replaceMode = false;
    state_.replacement.clear();
    state_.matches.clear();
    state_.activeMatch.reset();
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
    request_.options.caseSensitive = !request_.options.caseSensitive;
    ++state_.generation;
    evaluate(document);
}

void FindReplaceController::toggleWholeWord(
    const DocumentSnapshot& document) {
    request_.options.wholeWord = !request_.options.wholeWord;
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
    request_.options.selectionOnly = !request_.options.selectionOnly;
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
    state_.sourceRevision = document.revision;
    state_.query = request_.query;
    state_.options = request_.options;
    auto result = findTextMatches(document.text, request_);
    state_.matches = std::move(result.matches);
    state_.error = result.error;
    state_.message = std::move(result.message);
    state_.activeMatch =
        state_.matches.empty() ? std::nullopt : std::optional<std::size_t>{0};
}

void FindReplaceController::next() {
    if (!state_.matches.empty()) {
        state_.activeMatch =
            (state_.activeMatch.value_or(0) + 1) % state_.matches.size();
        ++state_.generation;
    }
}

void FindReplaceController::previous() {
    if (!state_.matches.empty()) {
        const auto current = state_.activeMatch.value_or(0);
        state_.activeMatch =
            (current + state_.matches.size() - 1) % state_.matches.size();
        ++state_.generation;
    }
}

FindReplaceOperationResult FindReplaceController::replaceCurrent(
    Document& document, DocumentHistory& history,
    const SelectionSet& selectionsBefore,
    const SelectionSet& selectionsAfter, std::string replacement,
    std::uint64_t timestampMs) {
    if (document.revision() != state_.sourceRevision) {
        return operationFailure(FindReplaceError::StaleRevision,
                                 document.revision(),
                                 "find result revision is stale");
    }
    if (!state_.activeMatch || state_.error != FindReplaceError::None) {
        return operationFailure(FindReplaceError::NoMatch,
                                 document.revision(), "no active match");
    }
    const auto match = state_.matches[*state_.activeMatch];
    if (match.begin == match.end && replacement.empty()) {
        return operationFailure(FindReplaceError::DocumentRejected,
                                 document.revision(),
                                 "replacement would not change the document");
    }
    EditTransaction transaction{
        state_.sourceRevision,
        {{match.begin, match.end.value() - match.begin.value(),
          std::move(replacement)}}};
    auto historyResult = history.applyEdit(
        document, transaction, selectionsBefore, selectionsAfter,
        HistoryEditKind::Other, timestampMs);
    if (!historyResult.accepted()) {
        return operationFailure(
            historyResult.error == HistoryError::StaleDocument
                ? FindReplaceError::StaleRevision
                : FindReplaceError::DocumentRejected,
            historyResult.revision, historyResult.message);
    }
    ++state_.generation;
    evaluate(document.snapshot());
    return {FindReplaceError::None, document.revision(), {}};
}

FindReplaceOperationResult FindReplaceController::replaceAll(
    Document& document, DocumentHistory& history,
    const SelectionSet& selectionsBefore,
    const SelectionSet& selectionsAfter, std::string replacement,
    std::uint64_t timestampMs) {
    if (document.revision() != state_.sourceRevision) {
        return operationFailure(FindReplaceError::StaleRevision,
                                 document.revision(),
                                 "find result revision is stale");
    }
    if (state_.matches.empty() || state_.error != FindReplaceError::None) {
        return operationFailure(FindReplaceError::NoMatch,
                                 document.revision(), "no matches");
    }
    EditTransaction transaction{state_.sourceRevision, {}};
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
    auto historyResult = history.applyEdit(
        document, transaction, selectionsBefore, selectionsAfter,
        HistoryEditKind::Other, timestampMs);
    if (!historyResult.accepted()) {
        return operationFailure(
            historyResult.error == HistoryError::StaleDocument
                ? FindReplaceError::StaleRevision
                : FindReplaceError::DocumentRejected,
            historyResult.revision, historyResult.message);
    }
    ++state_.generation;
    evaluate(document.snapshot());
    return {FindReplaceError::None, document.revision(), {}};
}

const FindReplaceViewState& FindReplaceController::viewState() const noexcept {
    return state_;
}

WorkspacePreviewResult previewWorkspaceReplace(
    const WorkspaceSnapshot& snapshot, const FindRequest& request,
    std::string replacement) {
    if (request.options.selectionOnly) {
        return {FindReplaceError::InvalidSelection, std::nullopt,
                "workspace replace cannot use a document selection"};
    }
    WorkspaceReplacePreview preview{snapshot.revision,
                                    request.query,
                                    std::move(replacement),
                                    request.options,
                                    {}};
    const auto perFileBudget =
        snapshot.files.empty()
            ? request.workBudget
            : request.workBudget /
                  static_cast<std::uint64_t>(snapshot.files.size());
    for (const auto& file : snapshot.files) {
        auto fileRequest = request;
        fileRequest.workBudget = perFileBudget;
        auto result = findTextMatches(file.text, fileRequest);
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

}  // namespace ssg
