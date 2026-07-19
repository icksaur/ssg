#include "ssg/syntax.h"

#include <ssg/startup_audit.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ssg {
namespace {

bool point_before_or_equal(const SyntaxPoint& left,
                           const SyntaxPoint& right) {
    return std::tie(left.row, left.column_byte) <=
           std::tie(right.row, right.column_byte);
}

SyntaxPoint point_at(std::string_view text, std::uint64_t offset) {
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

SyntaxPoint advance_point(SyntaxPoint point, std::string_view text) {
    for (const char value : text) {
        if (value == '\n') {
            point.row = LineIndex{point.row.value() + 1};
            point.column_byte = 0;
        } else {
            ++point.column_byte;
        }
    }
    return point;
}

bool valid_edits(const std::vector<SyntaxEdit>& edits,
                 std::string_view current_text,
                 std::string_view previous_text) {
    std::uint64_t previous_old_end = 0;
    std::uint64_t previous_cursor = 0;
    std::uint64_t current_cursor = 0;
    bool first = true;
    for (const auto& edit : edits) {
        const auto start = edit.start_byte.value();
        const auto old_end = edit.old_end_byte.value();
        const auto new_end = edit.new_end_byte.value();
        if (edit.start_byte > edit.old_end_byte ||
            edit.start_byte > edit.new_end_byte ||
            old_end > previous_text.size() ||
            !point_before_or_equal(edit.start_position,
                                   edit.old_end_position) ||
            !point_before_or_equal(edit.start_position,
                                   edit.new_end_position) ||
            (!first && start < previous_old_end) ||
            edit.start_position != point_at(previous_text, start) ||
            edit.old_end_position != point_at(previous_text, old_end)) {
            return false;
        }

        const auto unchanged_bytes = start - previous_cursor;
        const auto inserted_bytes = new_end - start;
        if (unchanged_bytes > current_text.size() - current_cursor ||
            previous_text.substr(previous_cursor, unchanged_bytes) !=
                current_text.substr(current_cursor, unchanged_bytes)) {
            return false;
        }
        current_cursor += unchanged_bytes;
        if (inserted_bytes > current_text.size() - current_cursor ||
            edit.new_end_position !=
                advance_point(edit.start_position,
                              current_text.substr(current_cursor,
                                                  inserted_bytes))) {
            return false;
        }
        current_cursor += inserted_bytes;
        previous_cursor = old_end;
        previous_old_end = old_end;
        first = false;
    }
    return previous_text.substr(previous_cursor) ==
           current_text.substr(current_cursor);
}

std::vector<LineIndentation> derive_indentation(std::string_view text,
                                                std::uint32_t tab_width) {
    std::vector<LineIndentation> result;
    std::size_t line_start = 0;
    std::uint64_t line = 0;
    while (line_start <= text.size()) {
        const auto newline = text.find('\n', line_start);
        const auto line_end =
            newline == std::string_view::npos ? text.size() : newline;
        auto content = line_start;
        std::uint32_t spaces = 0;
        std::uint32_t tabs = 0;
        std::uint32_t columns = 0;
        while (content < line_end &&
               (text[content] == ' ' || text[content] == '\t')) {
            if (text[content] == ' ') {
                ++spaces;
                ++columns;
            } else {
                ++tabs;
                columns += tab_width - columns % tab_width;
            }
            ++content;
        }
        result.push_back(
            {LineIndex{line}, ByteOffset{line_start}, ByteOffset{content},
             spaces, tabs, columns, content == line_end});
        if (newline == std::string_view::npos) {
            break;
        }
        line_start = newline + 1;
        ++line;
    }
    return result;
}

std::vector<SyntaxSpan> canonical_spans(std::uint64_t text_bytes,
                                        std::vector<SyntaxSpan> spans) {
    spans.erase(
        std::remove_if(spans.begin(), spans.end(),
                       [text_bytes](const SyntaxSpan& span) {
                           return span.begin >= span.end ||
                                  span.begin.value() >= text_bytes;
                       }),
        spans.end());
    for (auto& span : spans) {
        if (span.end.value() > text_bytes) {
            span.end = ByteOffset{text_bytes};
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
                              SyntaxScope::plain_text});
            cursor = span.begin.value();
        }
        result.push_back(
            {ByteOffset{cursor}, span.end, span.scope});
        cursor = span.end.value();
    }
    if (cursor < text_bytes) {
        result.push_back({ByteOffset{cursor}, ByteOffset{text_bytes},
                          SyntaxScope::plain_text});
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

std::vector<CommentToken> canonical_comment_tokens(
    std::uint64_t text_bytes, std::vector<CommentToken> tokens) {
    tokens.erase(
        std::remove_if(tokens.begin(), tokens.end(),
                       [text_bytes](const CommentToken& token) {
                           return token.range.begin >= token.range.end ||
                                  token.range.end.value() > text_bytes;
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

std::vector<CommentRange> canonical_comment_ranges(
    std::uint64_t text_bytes, std::vector<CommentRange> ranges) {
    ranges.erase(
        std::remove_if(ranges.begin(), ranges.end(),
                       [text_bytes](const CommentRange& range) {
                           return range.range.begin >= range.range.end ||
                                  range.range.end.value() > text_bytes;
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

ResolvedBrackets resolve_brackets(std::uint64_t text_bytes,
                                  std::vector<BracketToken> tokens) {
    tokens.erase(
        std::remove_if(tokens.begin(), tokens.end(),
                       [text_bytes](const BracketToken& token) {
                           return token.offset.value() >= text_bytes;
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
        if (token.role == BracketRole::open) {
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
                {token.offset, token.kind, BracketRole::close});
        }
    }
    for (const auto& open : stack) {
        result.unmatched.push_back(
            {open.token.offset, open.token.kind, BracketRole::open});
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

bool valid_state(const SyntaxViewState& state) {
    std::uint64_t cursor = 0;
    for (const auto& span : state.spans()) {
        if (span.begin.value() != cursor || span.begin >= span.end ||
            span.end.value() > state.text_bytes()) {
            return false;
        }
        cursor = span.end.value();
    }
    if (cursor != state.text_bytes()) {
        return false;
    }

    ByteOffset previous_open{0};
    bool first_pair = true;
    for (const auto& pair : state.bracket_pairs()) {
        if (pair.open >= pair.close ||
            pair.close.value() >= state.text_bytes() ||
            (!first_pair && pair.open <= previous_open)) {
            return false;
        }
        previous_open = pair.open;
        first_pair = false;
    }

    ByteOffset previous_unmatched{0};
    bool first_unmatched = true;
    for (const auto& bracket : state.unmatched_brackets()) {
        if (bracket.offset.value() >= state.text_bytes() ||
            (!first_unmatched && bracket.offset <= previous_unmatched)) {
            return false;
        }
        previous_unmatched = bracket.offset;
        first_unmatched = false;
    }

    SyntaxRange previous_token{ByteOffset{0}, ByteOffset{0}};
    bool first_token = true;
    for (const auto& token : state.comment_tokens()) {
        if (token.range.begin >= token.range.end ||
            token.range.end.value() > state.text_bytes() ||
            (!first_token &&
             std::tie(token.range.begin, token.range.end) <=
                 std::tie(previous_token.begin, previous_token.end))) {
            return false;
        }
        previous_token = token.range;
        first_token = false;
    }

    ByteOffset previous_range_end{0};
    for (const auto& range : state.comment_ranges()) {
        if (range.range.begin < previous_range_end ||
            range.range.begin >= range.range.end ||
            range.range.end.value() > state.text_bytes()) {
            return false;
        }
        previous_range_end = range.range.end;
    }

    if (state.indentation().empty()) {
        return false;
    }
    for (std::size_t index = 0; index < state.indentation().size(); ++index) {
        const auto& line = state.indentation()[index];
        if (line.line != LineIndex{index} ||
            line.line_start > line.content_start ||
            line.content_start.value() > state.text_bytes() ||
            (index != 0 &&
             line.line_start <= state.indentation()[index - 1].line_start)) {
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

LanguageId LanguageId::plain_text() {
    return LanguageId{"plain_text"};
}

bool LanguageId::is_plain_text() const noexcept {
    return value_ == "plain_text";
}

SyntaxParseRequest::SyntaxParseRequest(
    Revision revision, LanguageId language, std::string text,
    SyntaxParseHandle prior_parse, std::vector<SyntaxEdit> edits)
    : revision_(revision),
      language_(std::move(language)),
      text_(std::move(text)),
      prior_parse_(std::move(prior_parse)),
      edits_(std::move(edits)),
      cancelled_(std::make_shared<std::atomic_bool>(false)) {}

bool SyntaxParseRequest::cancelled() const noexcept {
    return cancelled_->load(std::memory_order_acquire);
}

void SyntaxParseRequest::cancel() const noexcept {
    cancelled_->store(true, std::memory_order_release);
}

SyntaxViewState::SyntaxViewState(
    Revision revision, LanguageId language, std::uint64_t text_bytes,
    std::vector<SyntaxSpan> spans, std::vector<SyntaxBracketPair> bracket_pairs,
    std::vector<UnmatchedBracket> unmatched_brackets,
    std::vector<CommentToken> comment_tokens,
    std::vector<CommentRange> comment_ranges,
    std::vector<LineIndentation> indentation)
    : revision_(revision),
      language_(std::move(language)),
      text_bytes_(text_bytes),
      spans_(std::move(spans)),
      bracket_pairs_(std::move(bracket_pairs)),
      unmatched_brackets_(std::move(unmatched_brackets)),
      comment_tokens_(std::move(comment_tokens)),
      comment_ranges_(std::move(comment_ranges)),
      indentation_(std::move(indentation)) {}

SyntaxViewState plain_text_syntax_view_state(
    Revision revision, LanguageId language, std::string_view text,
    std::uint32_t tab_width) {
    if (tab_width == 0) {
        throw std::invalid_argument{"tab width must be positive"};
    }
    std::vector<SyntaxSpan> spans;
    if (!text.empty()) {
        spans.push_back(
            {ByteOffset{0}, ByteOffset{text.size()}, SyntaxScope::plain_text});
    }
    return {revision,
            std::move(language),
            text.size(),
            std::move(spans),
            {},
            {},
            {},
            {},
            derive_indentation(text, tab_width)};
}

SyntaxViewState build_syntax_view_state(
    Revision revision, LanguageId language, std::string_view text,
    const SyntaxParseOutput& output, const SyntaxConfig& config) {
    if (config.tab_width == 0) {
        throw std::invalid_argument{"tab width must be positive"};
    }
    if (output.status != SyntaxParseStatus::parsed || !output.parse) {
        return plain_text_syntax_view_state(
            revision, std::move(language), text, config.tab_width);
    }
    auto brackets = resolve_brackets(text.size(), output.brackets);
    return {
        revision,
        std::move(language),
        text.size(),
        canonical_spans(text.size(), output.spans),
        std::move(brackets.pairs),
        std::move(brackets.unmatched),
        canonical_comment_tokens(text.size(), output.comment_tokens),
        canonical_comment_ranges(text.size(), output.comment_ranges),
        derive_indentation(text, config.tab_width),
    };
}

std::optional<ByteOffset> matching_bracket(const SyntaxViewState& state,
                                          ByteOffset offset) {
    for (const auto& pair : state.bracket_pairs()) {
        if (pair.open == offset) {
            return pair.close;
        }
        if (pair.close == offset) {
            return pair.open;
        }
    }
    return std::nullopt;
}

SyntaxScope scope_at(const SyntaxViewState& state, ByteOffset offset) {
    if (offset.value() >= state.text_bytes()) {
        return SyntaxScope::plain_text;
    }
    const auto span = std::upper_bound(
        state.spans().begin(), state.spans().end(), offset,
        [](ByteOffset position, const SyntaxSpan& candidate) {
            return position < candidate.begin;
        });
    if (span == state.spans().begin()) {
        return SyntaxScope::plain_text;
    }
    return std::prev(span)->scope;
}

SyntaxDelta::SyntaxDelta(
    Revision base_revision, Revision revision,
    std::optional<LanguageId> language,
    std::optional<std::uint64_t> text_bytes,
    std::optional<std::vector<SyntaxSpan>> spans,
    std::optional<std::vector<SyntaxBracketPair>> bracket_pairs,
    std::optional<std::vector<UnmatchedBracket>> unmatched_brackets,
    std::optional<std::vector<CommentToken>> comment_tokens,
    std::optional<std::vector<CommentRange>> comment_ranges,
    std::optional<std::vector<LineIndentation>> indentation)
    : base_revision_(base_revision),
      revision_(revision),
      language_(std::move(language)),
      text_bytes_(std::move(text_bytes)),
      spans_(std::move(spans)),
      bracket_pairs_(std::move(bracket_pairs)),
      unmatched_brackets_(std::move(unmatched_brackets)),
      comment_tokens_(std::move(comment_tokens)),
      comment_ranges_(std::move(comment_ranges)),
      indentation_(std::move(indentation)) {}

bool SyntaxDelta::empty() const noexcept {
    return !language_ && !text_bytes_ && !spans_ && !bracket_pairs_ &&
           !unmatched_brackets_ && !comment_tokens_ && !comment_ranges_ &&
           !indentation_;
}

SyntaxDelta derive_syntax_delta(const SyntaxViewState& base,
                                const SyntaxViewState& target) {
    return {
        base.revision(),
        target.revision(),
        changed(base.language(), target.language()),
        changed(base.text_bytes(), target.text_bytes()),
        changed(base.spans(), target.spans()),
        changed(base.bracket_pairs(), target.bracket_pairs()),
        changed(base.unmatched_brackets(), target.unmatched_brackets()),
        changed(base.comment_tokens(), target.comment_tokens()),
        changed(base.comment_ranges(), target.comment_ranges()),
        changed(base.indentation(), target.indentation()),
    };
}

SyntaxReplayResult replay_syntax_delta(const SyntaxViewState& base,
                                       const SyntaxDelta& delta) {
    if (base.revision() != delta.base_revision()) {
        return {std::nullopt, SyntaxReplayError::stale_revision};
    }
    if (delta.revision() < delta.base_revision() ||
        (delta.revision() == delta.base_revision() && !delta.empty())) {
        return {std::nullopt, SyntaxReplayError::malformed_delta};
    }

    SyntaxViewState result{
        delta.revision(),
        delta.language().value_or(base.language()),
        delta.text_bytes().value_or(base.text_bytes()),
        delta.spans().value_or(base.spans()),
        delta.bracket_pairs().value_or(base.bracket_pairs()),
        delta.unmatched_brackets().value_or(base.unmatched_brackets()),
        delta.comment_tokens().value_or(base.comment_tokens()),
        delta.comment_ranges().value_or(base.comment_ranges()),
        delta.indentation().value_or(base.indentation()),
    };
    if (!valid_state(result)) {
        return {std::nullopt, SyntaxReplayError::malformed_delta};
    }
    return {std::move(result), SyntaxReplayError::none};
}

SyntaxModel::SyntaxModel(std::shared_ptr<SyntaxParser> parser,
                         SyntaxConfig config)
    : parser_(std::move(parser)),
      config_(config),
      view_state_(plain_text_syntax_view_state(
          Revision{0}, LanguageId::plain_text(), {}, config.tab_width)) {
    // A real Tree-sitter grammar is only present when a parser is injected; the
    // plain-text fallback (parser == nullptr) constructs no grammar, so it is not
    // counted by the startup audit (I12 / doc/spec-fast-startup.md M10-2).
    if (parser_ != nullptr) {
        note_optional_construction(OptionalSubsystem::tree_sitter_grammar);
    }
    if (config_.tab_width == 0) {
        throw std::invalid_argument{"tab width must be positive"};
    }
}

bool SyntaxModel::has_grammar(const LanguageId& language) const noexcept {
    if (!parser_ || language.is_plain_text()) {
        return false;
    }
    try {
        return parser_->has_grammar(language);
    } catch (...) {
        return false;
    }
}

SyntaxParseRequestResult SyntaxModel::request(
    Revision revision, LanguageId language, std::string text,
    std::vector<SyntaxEdit> edits) {
    if (revision <= view_state_.revision() ||
        (pending_ && revision <= pending_->revision())) {
        return {nullptr, SyntaxRequestError::stale_revision};
    }
    cancel_pending();
    if (text.size() > config_.maximum_document_bytes) {
        return {nullptr, SyntaxRequestError::document_too_large};
    }
    const auto prior_parse =
        language == view_state_.language() ? accepted_parse_ : nullptr;
    if (!edits.empty() &&
        (!prior_parse || !valid_edits(edits, text, accepted_text_))) {
        return {nullptr, SyntaxRequestError::malformed_edits};
    }

    const auto request_prior =
        prior_parse && (text == accepted_text_ || !edits.empty())
            ? prior_parse
            : nullptr;
    pending_ = std::shared_ptr<const SyntaxParseRequest>(
        new SyntaxParseRequest{revision, std::move(language), std::move(text),
                               request_prior, std::move(edits)});
    return {pending_, SyntaxRequestError::none};
}

SyntaxParseOutput SyntaxModel::run(
    const SyntaxParseRequest& request) const {
    if (request.cancelled()) {
        return {.revision = request.revision(),
                .status = SyntaxParseStatus::cancelled};
    }
    if (!has_grammar(request.language())) {
        return {.revision = request.revision(),
                .status = SyntaxParseStatus::grammar_unavailable};
    }
    try {
        return parser_->parse(request);
    } catch (...) {
        return {.revision = request.revision(),
                .status = SyntaxParseStatus::failed};
    }
}

SyntaxAcceptResult SyntaxModel::accept(
    const std::shared_ptr<const SyntaxParseRequest>& request,
    const SyntaxParseOutput& output) {
    if (!request) {
        return {SyntaxAcceptError::unknown_request, false};
    }
    if (request->revision() <= view_state_.revision()) {
        return {SyntaxAcceptError::stale_revision, false};
    }
    if (request->cancelled()) {
        return {SyntaxAcceptError::cancelled, false};
    }
    if (request != pending_) {
        return {SyntaxAcceptError::unknown_request, false};
    }
    if (output.revision != request->revision()) {
        return {SyntaxAcceptError::malformed_output, false};
    }
    if (output.status == SyntaxParseStatus::cancelled) {
        return {SyntaxAcceptError::cancelled, false};
    }

    const bool fallback =
        output.status != SyntaxParseStatus::parsed || !output.parse;
    auto next = fallback
                    ? plain_text_syntax_view_state(
                          request->revision(), request->language(),
                          request->text(), config_.tab_width)
                    : build_syntax_view_state(
                          request->revision(), request->language(),
                          request->text(), output, config_);
    view_state_ = std::move(next);
    accepted_parse_ = fallback ? nullptr : output.parse;
    accepted_text_ = request->text();
    pending_.reset();
    return {SyntaxAcceptError::none, fallback};
}

void SyntaxModel::cancel_pending() noexcept {
    if (pending_) {
        pending_->cancel();
    }
}

} // namespace ssg
