#pragma once

#include <ssg/theme.h>
#include <ssg/types.h>

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

class LanguageId {
public:
    explicit LanguageId(std::string value);

    [[nodiscard]] static LanguageId plain_text();
    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    [[nodiscard]] bool is_plain_text() const noexcept;
    auto operator<=>(const LanguageId&) const = default;

private:
    std::string value_;
};

struct SyntaxPoint {
    LineIndex row;
    std::uint64_t column_byte = 0;

    friend bool operator==(const SyntaxPoint&, const SyntaxPoint&) = default;
};

struct SyntaxEdit {
    // Records are non-overlapping and sorted by start_byte in the prior text.
    // Applying them to an incremental parse tree therefore proceeds in reverse.
    ByteOffset start_byte;
    ByteOffset old_end_byte;
    ByteOffset new_end_byte;
    SyntaxPoint start_position;
    SyntaxPoint old_end_position;
    SyntaxPoint new_end_position;

    friend bool operator==(const SyntaxEdit&, const SyntaxEdit&) = default;
};

struct SyntaxRange {
    ByteOffset begin;
    ByteOffset end;

    friend bool operator==(const SyntaxRange&, const SyntaxRange&) = default;
};

struct SyntaxSpan {
    ByteOffset begin;
    ByteOffset end;
    SyntaxScope scope = SyntaxScope::PlainText;

    friend bool operator==(const SyntaxSpan&, const SyntaxSpan&) = default;
};

enum class BracketKind : std::uint8_t { Round, Square, Curly };
enum class BracketRole : std::uint8_t { Open, Close };

struct BracketToken {
    ByteOffset offset;
    BracketKind kind = BracketKind::Round;
    BracketRole role = BracketRole::Open;

    friend bool operator==(const BracketToken&, const BracketToken&) = default;
};

struct SyntaxBracketPair {
    ByteOffset open;
    ByteOffset close;
    BracketKind kind = BracketKind::Round;
    std::uint32_t depth = 0;

    friend bool operator==(const SyntaxBracketPair&,
                           const SyntaxBracketPair&) = default;
};

struct UnmatchedBracket {
    ByteOffset offset;
    BracketKind kind = BracketKind::Round;
    BracketRole role = BracketRole::Open;

    friend bool operator==(const UnmatchedBracket&,
                           const UnmatchedBracket&) = default;
};

enum class CommentKind : std::uint8_t { Line, Block };
enum class CommentTokenRole : std::uint8_t {
    Line,
    BlockOpen,
    BlockClose,
};

struct CommentToken {
    SyntaxRange range;
    CommentTokenRole role = CommentTokenRole::Line;

    friend bool operator==(const CommentToken&, const CommentToken&) = default;
};

struct CommentRange {
    SyntaxRange range;
    CommentKind kind = CommentKind::Line;

    friend bool operator==(const CommentRange&, const CommentRange&) = default;
};

struct LineIndentation {
    LineIndex line;
    ByteOffset line_start;
    ByteOffset content_start;
    std::uint32_t spaces = 0;
    std::uint32_t tabs = 0;
    std::uint32_t columns = 0;
    bool blank = true;

    friend bool operator==(const LineIndentation&,
                           const LineIndentation&) = default;
};

class OpaqueSyntaxParse {
public:
    virtual ~OpaqueSyntaxParse() = default;
};

using SyntaxParseHandle = std::shared_ptr<const OpaqueSyntaxParse>;

enum class SyntaxParseStatus : std::uint8_t {
    Parsed,
    GrammarUnavailable,
    Failed,
    Cancelled,
};

struct SyntaxParseOutput {
    Revision revision{0};
    SyntaxParseStatus status = SyntaxParseStatus::Failed;
    SyntaxParseHandle parse;
    std::vector<SyntaxSpan> spans;
    std::vector<BracketToken> brackets;
    std::vector<CommentToken> comment_tokens;
    std::vector<CommentRange> comment_ranges;
};

class SyntaxModel;

class SyntaxParseRequest {
public:
    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const LanguageId& language() const noexcept {
        return language_;
    }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] const SyntaxParseHandle& prior_parse() const noexcept {
        return prior_parse_;
    }
    [[nodiscard]] const std::vector<SyntaxEdit>& edits() const noexcept {
        return edits_;
    }
    [[nodiscard]] bool cancelled() const noexcept;
    void cancel() const noexcept;

private:
    friend class SyntaxModel;

    SyntaxParseRequest(Revision revision, LanguageId language, std::string text,
                       SyntaxParseHandle prior_parse,
                       std::vector<SyntaxEdit> edits);

    Revision revision_;
    LanguageId language_;
    std::string text_;
    SyntaxParseHandle prior_parse_;
    std::vector<SyntaxEdit> edits_;
    std::shared_ptr<std::atomic_bool> cancelled_;
};

class SyntaxParser {
public:
    virtual ~SyntaxParser() = default;
    [[nodiscard]] virtual bool has_grammar(const LanguageId& language) const = 0;
    [[nodiscard]] virtual SyntaxParseOutput parse(
        const SyntaxParseRequest& request) = 0;
};

struct SyntaxConfig {
    std::uint32_t tab_width = 4;
    std::size_t maximum_document_bytes = 64 * 1024 * 1024;
};

class SyntaxViewState {
public:
    SyntaxViewState(Revision revision, LanguageId language,
                    std::uint64_t text_bytes, std::vector<SyntaxSpan> spans,
                    std::vector<SyntaxBracketPair> bracket_pairs,
                    std::vector<UnmatchedBracket> unmatched_brackets,
                    std::vector<CommentToken> comment_tokens,
                    std::vector<CommentRange> comment_ranges,
                    std::vector<LineIndentation> indentation);

    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const LanguageId& language() const noexcept {
        return language_;
    }
    [[nodiscard]] std::uint64_t text_bytes() const noexcept {
        return text_bytes_;
    }
    [[nodiscard]] const std::vector<SyntaxSpan>& spans() const noexcept {
        return spans_;
    }
    [[nodiscard]] const std::vector<SyntaxBracketPair>& bracket_pairs() const noexcept {
        return bracket_pairs_;
    }
    [[nodiscard]] const std::vector<UnmatchedBracket>& unmatched_brackets()
        const noexcept {
        return unmatched_brackets_;
    }
    [[nodiscard]] const std::vector<CommentToken>& comment_tokens()
        const noexcept {
        return comment_tokens_;
    }
    [[nodiscard]] const std::vector<CommentRange>& comment_ranges()
        const noexcept {
        return comment_ranges_;
    }
    [[nodiscard]] const std::vector<LineIndentation>& indentation()
        const noexcept {
        return indentation_;
    }

    friend bool operator==(const SyntaxViewState&,
                           const SyntaxViewState&) = default;

private:
    Revision revision_;
    LanguageId language_;
    std::uint64_t text_bytes_;
    std::vector<SyntaxSpan> spans_;
    std::vector<SyntaxBracketPair> bracket_pairs_;
    std::vector<UnmatchedBracket> unmatched_brackets_;
    std::vector<CommentToken> comment_tokens_;
    std::vector<CommentRange> comment_ranges_;
    std::vector<LineIndentation> indentation_;
};

[[nodiscard]] SyntaxViewState plain_text_syntax_view_state(
    Revision revision, LanguageId language, std::string_view text,
    std::uint32_t tab_width);
[[nodiscard]] SyntaxViewState build_syntax_view_state(
    Revision revision, LanguageId language, std::string_view text,
    const SyntaxParseOutput& output, const SyntaxConfig& config);
[[nodiscard]] std::optional<ByteOffset> matching_bracket(
    const SyntaxViewState& state, ByteOffset offset);
[[nodiscard]] SyntaxScope scope_at(const SyntaxViewState& state,
                                   ByteOffset offset);

class SyntaxDelta {
public:
    SyntaxDelta(
        Revision base_revision, Revision revision,
        std::optional<LanguageId> language,
        std::optional<std::uint64_t> text_bytes,
        std::optional<std::vector<SyntaxSpan>> spans,
        std::optional<std::vector<SyntaxBracketPair>> bracket_pairs,
        std::optional<std::vector<UnmatchedBracket>> unmatched_brackets,
        std::optional<std::vector<CommentToken>> comment_tokens,
        std::optional<std::vector<CommentRange>> comment_ranges,
        std::optional<std::vector<LineIndentation>> indentation);

    [[nodiscard]] Revision base_revision() const noexcept {
        return base_revision_;
    }
    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const std::optional<LanguageId>& language() const noexcept {
        return language_;
    }
    [[nodiscard]] const std::optional<std::uint64_t>& text_bytes()
        const noexcept {
        return text_bytes_;
    }
    [[nodiscard]] const std::optional<std::vector<SyntaxSpan>>& spans()
        const noexcept {
        return spans_;
    }
    [[nodiscard]] const std::optional<std::vector<SyntaxBracketPair>>& bracket_pairs()
        const noexcept {
        return bracket_pairs_;
    }
    [[nodiscard]] const std::optional<std::vector<UnmatchedBracket>>&
    unmatched_brackets() const noexcept {
        return unmatched_brackets_;
    }
    [[nodiscard]] const std::optional<std::vector<CommentToken>>&
    comment_tokens() const noexcept {
        return comment_tokens_;
    }
    [[nodiscard]] const std::optional<std::vector<CommentRange>>&
    comment_ranges() const noexcept {
        return comment_ranges_;
    }
    [[nodiscard]] const std::optional<std::vector<LineIndentation>>&
    indentation() const noexcept {
        return indentation_;
    }
    [[nodiscard]] bool empty() const noexcept;

    friend bool operator==(const SyntaxDelta&, const SyntaxDelta&) = default;

private:
    Revision base_revision_;
    Revision revision_;
    std::optional<LanguageId> language_;
    std::optional<std::uint64_t> text_bytes_;
    std::optional<std::vector<SyntaxSpan>> spans_;
    std::optional<std::vector<SyntaxBracketPair>> bracket_pairs_;
    std::optional<std::vector<UnmatchedBracket>> unmatched_brackets_;
    std::optional<std::vector<CommentToken>> comment_tokens_;
    std::optional<std::vector<CommentRange>> comment_ranges_;
    std::optional<std::vector<LineIndentation>> indentation_;
};

[[nodiscard]] SyntaxDelta derive_syntax_delta(const SyntaxViewState& base,
                                              const SyntaxViewState& target);

enum class SyntaxReplayError : std::uint8_t {
    None,
    StaleRevision,
    MalformedDelta,
};

struct SyntaxReplayResult {
    std::optional<SyntaxViewState> state;
    SyntaxReplayError error = SyntaxReplayError::None;

    [[nodiscard]] bool accepted() const noexcept { return state.has_value(); }
};

[[nodiscard]] SyntaxReplayResult replay_syntax_delta(
    const SyntaxViewState& base, const SyntaxDelta& delta);

enum class SyntaxRequestError : std::uint8_t {
    None,
    StaleRevision,
    DocumentTooLarge,
    MalformedEdits,
};

struct SyntaxParseRequestResult {
    std::shared_ptr<const SyntaxParseRequest> request;
    SyntaxRequestError error = SyntaxRequestError::None;

    [[nodiscard]] bool accepted() const noexcept {
        return request != nullptr && error == SyntaxRequestError::None;
    }
};

enum class SyntaxAcceptError : std::uint8_t {
    None,
    StaleRevision,
    Cancelled,
    UnknownRequest,
    MalformedOutput,
};

struct SyntaxAcceptResult {
    SyntaxAcceptError error = SyntaxAcceptError::None;
    bool used_fallback = false;

    [[nodiscard]] bool accepted() const noexcept {
        return error == SyntaxAcceptError::None;
    }
};

class SyntaxModel {
public:
    explicit SyntaxModel(std::shared_ptr<SyntaxParser> parser = nullptr,
                         SyntaxConfig config = {});

    [[nodiscard]] bool has_parser() const noexcept {
        return parser_ != nullptr;
    }
    [[nodiscard]] bool has_grammar(const LanguageId& language) const noexcept;
    [[nodiscard]] SyntaxParseRequestResult request(
        Revision revision, LanguageId language, std::string text,
        std::vector<SyntaxEdit> edits = {});
    [[nodiscard]] SyntaxParseOutput run(
        const SyntaxParseRequest& request) const;
    [[nodiscard]] SyntaxAcceptResult accept(
        const std::shared_ptr<const SyntaxParseRequest>& request,
        const SyntaxParseOutput& output);
    void cancel_pending() noexcept;

    [[nodiscard]] const SyntaxViewState& view_state() const noexcept {
        return view_state_;
    }

private:
    std::shared_ptr<SyntaxParser> parser_;
    SyntaxConfig config_;
    SyntaxViewState view_state_;
    SyntaxParseHandle accepted_parse_;
    std::string accepted_text_;
    std::shared_ptr<const SyntaxParseRequest> pending_;
};

} // namespace ssg
