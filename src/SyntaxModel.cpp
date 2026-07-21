#include "ssg/SyntaxModel.h"

#include <ssg/startup_audit.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace ssg {
namespace {

struct LanguageExtension {
    std::string_view extension;
    std::string_view language;
};

constexpr std::array<LanguageExtension, 15> kLanguageExtensions{{
    {".c", "c"},
    {".h", "c"},
    {".cc", "cpp"},
    {".cpp", "cpp"},
    {".cxx", "cpp"},
    {".hpp", "cpp"},
    {".hh", "cpp"},
    {".hxx", "cpp"},
    {".js", "javascript"},
    {".mjs", "javascript"},
    {".cjs", "javascript"},
    {".ts", "typescript"},
    {".tsx", "typescript"},
    {".cs", "csharp"},
    {".lua", "lua"},
}};

std::string lowerAscii(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char ch : value) {
        lowered.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

std::string_view fileName(std::string_view path) {
    auto separator = path.find_last_of("/\\");
    if (separator == std::string_view::npos) {
        return path;
    }
    return path.substr(separator + 1);
}

bool pointBeforeOrEqual(const SyntaxPoint& left,
                           const SyntaxPoint& right) {
    return std::tie(left.row, left.columnByte) <=
           std::tie(right.row, right.columnByte);
}

SyntaxPoint pointAt(std::string_view text, std::uint64_t offset) {
    std::uint64_t row = 0;
    std::uint64_t column = 0;
    for (std::uint64_t index = 0; index < offset; ++index) {
        if (text[index] == '\n') {
            ++row;
            column = 0;
        } else {
            ++column;
        }
    }
    return {LineIndex{row}, column};
}

SyntaxPoint advancePoint(SyntaxPoint point, std::string_view text) {
    for (const char value : text) {
        if (value == '\n') {
            point.row = LineIndex{point.row.value() + 1};
            point.columnByte = 0;
        } else {
            ++point.columnByte;
        }
    }
    return point;
}

bool validEdits(const std::vector<SyntaxEdit>& edits,
                 std::string_view currentText,
                 std::string_view previousText) {
    std::uint64_t previousOldEnd = 0;
    std::uint64_t previousCursor = 0;
    std::uint64_t currentCursor = 0;
    bool first = true;
    for (const auto& edit : edits) {
        const auto start = edit.startByte.value();
        const auto oldEnd = edit.oldEndByte.value();
        const auto newEnd = edit.newEndByte.value();
        if (edit.startByte > edit.oldEndByte ||
            edit.startByte > edit.newEndByte ||
            oldEnd > previousText.size() ||
            !pointBeforeOrEqual(edit.startPosition,
                                   edit.oldEndPosition) ||
            !pointBeforeOrEqual(edit.startPosition,
                                   edit.newEndPosition) ||
            (!first && start < previousOldEnd) ||
            edit.startPosition != pointAt(previousText, start) ||
            edit.oldEndPosition != pointAt(previousText, oldEnd)) {
            return false;
        }

        const auto unchangedBytes = start - previousCursor;
        const auto insertedBytes = newEnd - start;
        if (unchangedBytes > currentText.size() - currentCursor ||
            previousText.substr(previousCursor, unchangedBytes) !=
                currentText.substr(currentCursor, unchangedBytes)) {
            return false;
        }
        currentCursor += unchangedBytes;
        if (insertedBytes > currentText.size() - currentCursor ||
            edit.newEndPosition !=
                advancePoint(edit.startPosition,
                              currentText.substr(currentCursor,
                                                  insertedBytes))) {
            return false;
        }
        currentCursor += insertedBytes;
        previousCursor = oldEnd;
        previousOldEnd = oldEnd;
        first = false;
    }
    return previousText.substr(previousCursor) ==
           currentText.substr(currentCursor);
}

std::vector<LineIndentation> deriveIndentation(std::string_view text,
                                                std::uint32_t tabWidth) {
    std::vector<LineIndentation> result;
    std::size_t lineStart = 0;
    std::uint64_t line = 0;
    while (lineStart <= text.size()) {
        const auto newline = text.find('\n', lineStart);
        const auto lineEnd =
            newline == std::string_view::npos ? text.size() : newline;
        auto content = lineStart;
        std::uint32_t spaces = 0;
        std::uint32_t tabs = 0;
        std::uint32_t columns = 0;
        while (content < lineEnd &&
               (text[content] == ' ' || text[content] == '\t')) {
            if (text[content] == ' ') {
                ++spaces;
                ++columns;
            } else {
                ++tabs;
                columns += tabWidth - columns % tabWidth;
            }
            ++content;
        }
        result.push_back(
            {LineIndex{line}, ByteOffset{lineStart}, ByteOffset{content},
             spaces, tabs, columns, content == lineEnd});
        if (newline == std::string_view::npos) {
            break;
        }
        lineStart = newline + 1;
        ++line;
    }
    return result;
}

std::vector<SyntaxSpan> canonicalSpans(std::uint64_t textBytes,
                                        std::vector<SyntaxSpan> spans) {
    spans.erase(
        std::remove_if(spans.begin(), spans.end(),
                       [textBytes](const SyntaxSpan& span) {
                           return span.begin >= span.end ||
                                  span.begin.value() >= textBytes;
                       }),
        spans.end());
    for (auto& span : spans) {
        if (span.end.value() > textBytes) {
            span.end = ByteOffset{textBytes};
        }
    }
    std::sort(spans.begin(), spans.end(),
              [](const SyntaxSpan& left, const SyntaxSpan& right) {
                  return std::tie(left.begin, left.end, left.scope) <
                         std::tie(right.begin, right.end, right.scope);
              });

    std::vector<SyntaxSpan> result;
    std::uint64_t cursor = 0;
    for (const auto& span : spans) {
        if (span.end.value() <= cursor) {
            continue;
        }
        if (span.begin.value() > cursor) {
            result.push_back({ByteOffset{cursor}, span.begin,
                              SyntaxScope::PlainText});
            cursor = span.begin.value();
        }
        result.push_back(
            {ByteOffset{cursor}, span.end, span.scope});
        cursor = span.end.value();
    }
    if (cursor < textBytes) {
        result.push_back({ByteOffset{cursor}, ByteOffset{textBytes},
                          SyntaxScope::PlainText});
    }

    std::vector<SyntaxSpan> merged;
    for (const auto& span : result) {
        if (!merged.empty() && merged.back().scope == span.scope &&
            merged.back().end == span.begin) {
            merged.back().end = span.end;
        } else {
            merged.push_back(span);
        }
    }
    return merged;
}

std::vector<CommentToken> canonicalCommentTokens(
    std::uint64_t textBytes, std::vector<CommentToken> tokens) {
    tokens.erase(
        std::remove_if(tokens.begin(), tokens.end(),
                       [textBytes](const CommentToken& token) {
                           return token.range.begin >= token.range.end ||
                                  token.range.end.value() > textBytes;
                       }),
        tokens.end());
    std::sort(tokens.begin(), tokens.end(),
              [](const CommentToken& left, const CommentToken& right) {
                  return std::tie(left.range.begin, left.range.end, left.role) <
                         std::tie(right.range.begin, right.range.end,
                                  right.role);
              });
    tokens.erase(
        std::unique(tokens.begin(), tokens.end(),
                    [](const CommentToken& left, const CommentToken& right) {
                        return left.range == right.range;
                    }),
        tokens.end());
    return tokens;
}

std::vector<CommentRange> canonicalCommentRanges(
    std::uint64_t textBytes, std::vector<CommentRange> ranges) {
    ranges.erase(
        std::remove_if(ranges.begin(), ranges.end(),
                       [textBytes](const CommentRange& range) {
                           return range.range.begin >= range.range.end ||
                                  range.range.end.value() > textBytes;
                       }),
        ranges.end());
    std::sort(ranges.begin(), ranges.end(),
              [](const CommentRange& left, const CommentRange& right) {
                  return std::tie(left.range.begin, left.range.end, left.kind) <
                         std::tie(right.range.begin, right.range.end,
                                  right.kind);
              });
    std::vector<CommentRange> result;
    for (auto range : ranges) {
        if (!result.empty() &&
            range.range.begin < result.back().range.end) {
            range.range.begin = result.back().range.end;
        }
        if (range.range.begin < range.range.end) {
            result.push_back(range);
        }
    }
    return result;
}

struct ResolvedBrackets {
    std::vector<SyntaxBracketPair> pairs;
    std::vector<UnmatchedBracket> unmatched;
};

ResolvedBrackets resolveBrackets(std::uint64_t textBytes,
                                  std::vector<BracketToken> tokens) {
    tokens.erase(
        std::remove_if(tokens.begin(), tokens.end(),
                       [textBytes](const BracketToken& token) {
                           return token.offset.value() >= textBytes;
                       }),
        tokens.end());
    std::sort(tokens.begin(), tokens.end(),
              [](const BracketToken& left, const BracketToken& right) {
                  return std::tie(left.offset, left.role, left.kind) <
                         std::tie(right.offset, right.role, right.kind);
              });
    tokens.erase(
        std::unique(tokens.begin(), tokens.end(),
                    [](const BracketToken& left, const BracketToken& right) {
                        return left.offset == right.offset;
                    }),
        tokens.end());

    struct OpenBracket {
        BracketToken token;
        std::uint32_t depth;
    };
    std::vector<OpenBracket> stack;
    ResolvedBrackets result;
    for (const auto& token : tokens) {
        if (token.role == BracketRole::Open) {
            stack.push_back(
                {token, static_cast<std::uint32_t>(stack.size())});
            continue;
        }
        if (!stack.empty() && stack.back().token.kind == token.kind) {
            result.pairs.push_back(
                {stack.back().token.offset, token.offset, token.kind,
                 stack.back().depth});
            stack.pop_back();
        } else {
            result.unmatched.push_back(
                {token.offset, token.kind, BracketRole::Close});
        }
    }
    for (const auto& open : stack) {
        result.unmatched.push_back(
            {open.token.offset, open.token.kind, BracketRole::Open});
    }
    std::sort(result.pairs.begin(), result.pairs.end(),
              [](const SyntaxBracketPair& left, const SyntaxBracketPair& right) {
                  return left.open < right.open;
              });
    std::sort(result.unmatched.begin(), result.unmatched.end(),
              [](const UnmatchedBracket& left,
                 const UnmatchedBracket& right) {
                  return left.offset < right.offset;
              });
    return result;
}

template <typename T>
std::optional<T> changed(const T& before, const T& after) {
    if (before == after) {
        return std::nullopt;
    }
    return after;
}

bool validState(const SyntaxViewState& state) {
    std::uint64_t cursor = 0;
    for (const auto& span : state.spans()) {
        if (span.begin.value() != cursor || span.begin >= span.end ||
            span.end.value() > state.textBytes()) {
            return false;
        }
        cursor = span.end.value();
    }
    if (cursor != state.textBytes()) {
        return false;
    }

    ByteOffset previousOpen{0};
    bool firstPair = true;
    for (const auto& pair : state.bracketPairs()) {
        if (pair.open >= pair.close ||
            pair.close.value() >= state.textBytes() ||
            (!firstPair && pair.open <= previousOpen)) {
            return false;
        }
        previousOpen = pair.open;
        firstPair = false;
    }

    ByteOffset previousUnmatched{0};
    bool firstUnmatched = true;
    for (const auto& bracket : state.unmatchedBrackets()) {
        if (bracket.offset.value() >= state.textBytes() ||
            (!firstUnmatched && bracket.offset <= previousUnmatched)) {
            return false;
        }
        previousUnmatched = bracket.offset;
        firstUnmatched = false;
    }

    SyntaxRange previousToken{ByteOffset{0}, ByteOffset{0}};
    bool firstToken = true;
    for (const auto& token : state.commentTokens()) {
        if (token.range.begin >= token.range.end ||
            token.range.end.value() > state.textBytes() ||
            (!firstToken &&
             std::tie(token.range.begin, token.range.end) <=
                 std::tie(previousToken.begin, previousToken.end))) {
            return false;
        }
        previousToken = token.range;
        firstToken = false;
    }

    ByteOffset previousRangeEnd{0};
    for (const auto& range : state.commentRanges()) {
        if (range.range.begin < previousRangeEnd ||
            range.range.begin >= range.range.end ||
            range.range.end.value() > state.textBytes()) {
            return false;
        }
        previousRangeEnd = range.range.end;
    }

    if (state.indentation().empty()) {
        return false;
    }
    for (std::size_t index = 0; index < state.indentation().size(); ++index) {
        const auto& line = state.indentation()[index];
        if (line.line != LineIndex{index} ||
            line.lineStart > line.contentStart ||
            line.contentStart.value() > state.textBytes() ||
            (index != 0 &&
             line.lineStart <= state.indentation()[index - 1].lineStart)) {
            return false;
        }
    }
    return true;
}

} // namespace

LanguageId::LanguageId(std::string value) : value_(std::move(value)) {
    if (value_.empty()) {
        throw std::invalid_argument{"language id must not be empty"};
    }
}

LanguageId LanguageId::plainText() {
    return LanguageId{"plain_text"};
}

LanguageId LanguageId::c() {
    return LanguageId{"c"};
}

LanguageId LanguageId::cpp() {
    return LanguageId{"cpp"};
}

LanguageId LanguageId::javascript() {
    return LanguageId{"javascript"};
}

LanguageId LanguageId::typescript() {
    return LanguageId{"typescript"};
}

LanguageId LanguageId::csharp() {
    return LanguageId{"csharp"};
}

LanguageId LanguageId::lua() {
    return LanguageId{"lua"};
}

LanguageId LanguageId::fromPath(std::string_view path) {
    auto name = fileName(path);
    auto separator = name.find_last_of('.');
    if (separator == std::string_view::npos || separator == 0) {
        return plainText();
    }
    auto extension = lowerAscii(name.substr(separator));
    const auto found = std::find_if(
        kLanguageExtensions.begin(), kLanguageExtensions.end(),
        [&](const LanguageExtension& mapping) {
            return mapping.extension == extension;
        });
    if (found == kLanguageExtensions.end()) {
        return plainText();
    }
    return LanguageId{std::string{found->language}};
}

bool LanguageId::isPlainText() const noexcept {
    return value_ == "plain_text";
}

SyntaxParseRequest::SyntaxParseRequest(
    Revision revision, LanguageId language, std::string text,
    SyntaxParseHandle priorParse, std::vector<SyntaxEdit> edits)
    : revision_(revision),
      language_(std::move(language)),
      text_(std::move(text)),
      priorParse_(std::move(priorParse)),
      edits_(std::move(edits)),
      cancelled_(std::make_shared<std::atomic_bool>(false)) {}

bool SyntaxParseRequest::cancelled() const noexcept {
    return cancelled_->load(std::memory_order_acquire);
}

void SyntaxParseRequest::cancel() const noexcept {
    cancelled_->store(true, std::memory_order_release);
}

SyntaxViewState::SyntaxViewState(
    Revision revision, LanguageId language, std::uint64_t textBytes,
    std::vector<SyntaxSpan> spans, std::vector<SyntaxBracketPair> bracketPairs,
    std::vector<UnmatchedBracket> unmatchedBrackets,
    std::vector<CommentToken> commentTokens,
    std::vector<CommentRange> commentRanges,
    std::vector<LineIndentation> indentation)
    : revision_(revision),
      language_(std::move(language)),
      textBytes_(textBytes),
      spans_(std::move(spans)),
      bracketPairs_(std::move(bracketPairs)),
      unmatchedBrackets_(std::move(unmatchedBrackets)),
      commentTokens_(std::move(commentTokens)),
      commentRanges_(std::move(commentRanges)),
      indentation_(std::move(indentation)) {}

SyntaxViewState plainTextSyntaxViewState(
    Revision revision, LanguageId language, std::string_view text,
    std::uint32_t tabWidth) {
    if (tabWidth == 0) {
        throw std::invalid_argument{"tab width must be positive"};
    }
    std::vector<SyntaxSpan> spans;
    if (!text.empty()) {
        spans.push_back(
            {ByteOffset{0}, ByteOffset{text.size()}, SyntaxScope::PlainText});
    }
    return {revision,
            std::move(language),
            text.size(),
            std::move(spans),
            {},
            {},
            {},
            {},
            deriveIndentation(text, tabWidth)};
}

SyntaxViewState buildSyntaxViewState(
    Revision revision, LanguageId language, std::string_view text,
    const SyntaxParseOutput& output, const SyntaxConfig& config) {
    if (config.tabWidth == 0) {
        throw std::invalid_argument{"tab width must be positive"};
    }
    if (output.status != SyntaxParseStatus::Parsed || !output.parse) {
        return plainTextSyntaxViewState(
            revision, std::move(language), text, config.tabWidth);
    }
    auto brackets = resolveBrackets(text.size(), output.brackets);
    return {
        revision,
        std::move(language),
        text.size(),
        canonicalSpans(text.size(), output.spans),
        std::move(brackets.pairs),
        std::move(brackets.unmatched),
        canonicalCommentTokens(text.size(), output.commentTokens),
        canonicalCommentRanges(text.size(), output.commentRanges),
        deriveIndentation(text, config.tabWidth),
    };
}

std::optional<ByteOffset> matchingBracket(const SyntaxViewState& state,
                                          ByteOffset offset) {
    for (const auto& pair : state.bracketPairs()) {
        if (pair.open == offset) {
            return pair.close;
        }
        if (pair.close == offset) {
            return pair.open;
        }
    }
    return std::nullopt;
}

SyntaxScope scopeAt(const SyntaxViewState& state, ByteOffset offset) {
    if (offset.value() >= state.textBytes()) {
        return SyntaxScope::PlainText;
    }
    const auto span = std::upper_bound(
        state.spans().begin(), state.spans().end(), offset,
        [](ByteOffset position, const SyntaxSpan& candidate) {
            return position < candidate.begin;
        });
    if (span == state.spans().begin()) {
        return SyntaxScope::PlainText;
    }
    return std::prev(span)->scope;
}

SyntaxDelta::SyntaxDelta(
    Revision baseRevision, Revision revision,
    std::optional<LanguageId> language,
    std::optional<std::uint64_t> textBytes,
    std::optional<std::vector<SyntaxSpan>> spans,
    std::optional<std::vector<SyntaxBracketPair>> bracketPairs,
    std::optional<std::vector<UnmatchedBracket>> unmatchedBrackets,
    std::optional<std::vector<CommentToken>> commentTokens,
    std::optional<std::vector<CommentRange>> commentRanges,
    std::optional<std::vector<LineIndentation>> indentation)
    : baseRevision_(baseRevision),
      revision_(revision),
      language_(std::move(language)),
      textBytes_(std::move(textBytes)),
      spans_(std::move(spans)),
      bracketPairs_(std::move(bracketPairs)),
      unmatchedBrackets_(std::move(unmatchedBrackets)),
      commentTokens_(std::move(commentTokens)),
      commentRanges_(std::move(commentRanges)),
      indentation_(std::move(indentation)) {}

bool SyntaxDelta::empty() const noexcept {
    return !language_ && !textBytes_ && !spans_ && !bracketPairs_ &&
           !unmatchedBrackets_ && !commentTokens_ && !commentRanges_ &&
           !indentation_;
}

SyntaxDelta SyntaxDeltaCodec::derive(const SyntaxViewState& base,
                                const SyntaxViewState& target) {
    return {
        base.revision(),
        target.revision(),
        changed(base.language(), target.language()),
        changed(base.textBytes(), target.textBytes()),
        changed(base.spans(), target.spans()),
        changed(base.bracketPairs(), target.bracketPairs()),
        changed(base.unmatchedBrackets(), target.unmatchedBrackets()),
        changed(base.commentTokens(), target.commentTokens()),
        changed(base.commentRanges(), target.commentRanges()),
        changed(base.indentation(), target.indentation()),
    };
}

SyntaxReplayResult SyntaxDeltaCodec::replay(const SyntaxViewState& base,
                                       const SyntaxDelta& delta) {
    if (base.revision() != delta.baseRevision()) {
        return {std::nullopt, SyntaxReplayError::StaleRevision};
    }
    if (delta.revision() < delta.baseRevision() ||
        (delta.revision() == delta.baseRevision() && !delta.empty())) {
        return {std::nullopt, SyntaxReplayError::MalformedDelta};
    }

    SyntaxViewState result{
        delta.revision(),
        delta.language().value_or(base.language()),
        delta.textBytes().value_or(base.textBytes()),
        delta.spans().value_or(base.spans()),
        delta.bracketPairs().value_or(base.bracketPairs()),
        delta.unmatchedBrackets().value_or(base.unmatchedBrackets()),
        delta.commentTokens().value_or(base.commentTokens()),
        delta.commentRanges().value_or(base.commentRanges()),
        delta.indentation().value_or(base.indentation()),
    };
    if (!validState(result)) {
        return {std::nullopt, SyntaxReplayError::MalformedDelta};
    }
    return {std::move(result), SyntaxReplayError::None};
}

SyntaxModel::SyntaxModel(std::shared_ptr<SyntaxParser> parser,
                         SyntaxConfig config)
    : parser_(std::move(parser)),
      config_(config),
      viewState_(plainTextSyntaxViewState(
          Revision{0}, LanguageId::plainText(), {}, config.tabWidth)) {
    // A real Tree-sitter grammar is only present when a parser is injected; the
    // plain-text fallback (parser == nullptr) constructs no grammar, so it is not
    // counted by the startup audit (I12 / doc/spec-fast-startup.md M10-2).
    if (parser_ != nullptr) {
        noteOptionalConstruction(OptionalSubsystem::TreeSitterGrammar);
    }
    if (config_.tabWidth == 0) {
        throw std::invalid_argument{"tab width must be positive"};
    }
}

bool SyntaxModel::hasGrammar(const LanguageId& language) const noexcept {
    if (!parser_ || language.isPlainText()) {
        return false;
    }
    try {
        return parser_->hasGrammar(language);
    } catch (...) {
        return false;
    }
}

SyntaxParseRequestResult SyntaxModel::request(
    Revision revision, LanguageId language, std::string text,
    std::vector<SyntaxEdit> edits) {
    if (revision <= viewState_.revision() ||
        (pending_ && revision <= pending_->revision())) {
        return {nullptr, SyntaxRequestError::StaleRevision};
    }
    cancelPending();
    if (text.size() > config_.maximumDocumentBytes) {
        return {nullptr, SyntaxRequestError::DocumentTooLarge};
    }
    const auto priorParse =
        language == viewState_.language() ? acceptedParse_ : nullptr;
    if (!edits.empty() &&
        (!priorParse || !validEdits(edits, text, acceptedText_))) {
        return {nullptr, SyntaxRequestError::MalformedEdits};
    }

    const auto requestPrior =
        priorParse && (text == acceptedText_ || !edits.empty())
            ? priorParse
            : nullptr;
    pending_ = std::shared_ptr<const SyntaxParseRequest>(
        new SyntaxParseRequest{revision, std::move(language), std::move(text),
                               requestPrior, std::move(edits)});
    return {pending_, SyntaxRequestError::None};
}

SyntaxParseOutput SyntaxModel::run(
    const SyntaxParseRequest& request) const {
    if (request.cancelled()) {
        return {.revision = request.revision(),
                .status = SyntaxParseStatus::Cancelled};
    }
    if (!hasGrammar(request.language())) {
        return {.revision = request.revision(),
                .status = SyntaxParseStatus::GrammarUnavailable};
    }
    try {
        return parser_->parse(request);
    } catch (...) {
        return {.revision = request.revision(),
                .status = SyntaxParseStatus::Failed};
    }
}

SyntaxAcceptResult SyntaxModel::accept(
    const std::shared_ptr<const SyntaxParseRequest>& request,
    const SyntaxParseOutput& output) {
    if (!request) {
        return {SyntaxAcceptError::UnknownRequest, false};
    }
    if (request->revision() <= viewState_.revision()) {
        return {SyntaxAcceptError::StaleRevision, false};
    }
    if (request->cancelled()) {
        return {SyntaxAcceptError::Cancelled, false};
    }
    if (request != pending_) {
        return {SyntaxAcceptError::UnknownRequest, false};
    }
    if (output.revision != request->revision()) {
        return {SyntaxAcceptError::MalformedOutput, false};
    }
    if (output.status == SyntaxParseStatus::Cancelled) {
        return {SyntaxAcceptError::Cancelled, false};
    }

    const bool fallback =
        output.status != SyntaxParseStatus::Parsed || !output.parse;
    auto next = fallback
                    ? plainTextSyntaxViewState(
                          request->revision(), request->language(),
                          request->text(), config_.tabWidth)
                    : buildSyntaxViewState(
                          request->revision(), request->language(),
                          request->text(), output, config_);
    viewState_ = std::move(next);
    acceptedParse_ = fallback ? nullptr : output.parse;
    acceptedText_ = request->text();
    pending_.reset();
    return {SyntaxAcceptError::None, fallback};
}

void SyntaxModel::cancelPending() noexcept {
    if (pending_) {
        pending_->cancel();
    }
}

} // namespace ssg
