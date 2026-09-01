#pragma once

#include <ssg/detail/generated/semantic_wire_manifest.h>

#include <ssg/Theme.h>
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

    [[nodiscard]] static LanguageId plainText();
    [[nodiscard]] static LanguageId c();
    [[nodiscard]] static LanguageId cpp();
    [[nodiscard]] static LanguageId javascript();
    [[nodiscard]] static LanguageId typescript();
    [[nodiscard]] static LanguageId csharp();
    [[nodiscard]] static LanguageId lua();
    [[nodiscard]] static LanguageId fromPath(std::string_view path);
    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    [[nodiscard]] bool isPlainText() const noexcept;
    auto operator<=>(const LanguageId&) const = default;

private:
    std::string value_;
};

struct SyntaxPoint {
    LineIndex row;
    std::uint64_t columnByte = 0;

    friend bool operator==(const SyntaxPoint&, const SyntaxPoint&) = default;
};

struct SyntaxEdit {
    // Records are non-overlapping and sorted by start_byte in the prior text.
    // Applying them to an incremental parse tree therefore proceeds in reverse.
    ByteOffset startByte;
    ByteOffset oldEndByte;
    ByteOffset newEndByte;
    SyntaxPoint startPosition;
    SyntaxPoint oldEndPosition;
    SyntaxPoint newEndPosition;

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

enum class BracketKind : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_BRACKET_KIND_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_BRACKET_KIND_ENUMERATORS
enum class BracketRole : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_BRACKET_ROLE_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_BRACKET_ROLE_ENUMERATORS

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

enum class CommentKind : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_COMMENT_KIND_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_COMMENT_KIND_ENUMERATORS
enum class CommentTokenRole : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_COMMENT_TOKEN_ROLE_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_COMMENT_TOKEN_ROLE_ENUMERATORS

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
    ByteOffset lineStart;
    ByteOffset contentStart;
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
    std::vector<CommentToken> commentTokens;
    std::vector<CommentRange> commentRanges;
};

class SyntaxModel;

class SyntaxParseRequest {
public:
    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const LanguageId& language() const noexcept {
        return language_;
    }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] const SyntaxParseHandle& priorParse() const noexcept {
        return priorParse_;
    }
    [[nodiscard]] const std::vector<SyntaxEdit>& edits() const noexcept {
        return edits_;
    }
    [[nodiscard]] bool cancelled() const noexcept;
    void cancel() const noexcept;

private:
    friend class SyntaxModel;

    SyntaxParseRequest(Revision revision, LanguageId language, std::string text,
                       SyntaxParseHandle priorParse,
                       std::vector<SyntaxEdit> edits);

    Revision revision_;
    LanguageId language_;
    std::string text_;
    SyntaxParseHandle priorParse_;
    std::vector<SyntaxEdit> edits_;
    std::shared_ptr<std::atomic_bool> cancelled_;
};

class SyntaxParser {
public:
    virtual ~SyntaxParser() = default;
    [[nodiscard]] virtual bool hasGrammar(const LanguageId& language) const = 0;
    [[nodiscard]] virtual SyntaxParseOutput parse(
        const SyntaxParseRequest& request) = 0;
};

struct SyntaxConfig {
    std::uint32_t tabWidth = 4;
    std::size_t maximumDocumentBytes = 64 * 1024 * 1024;
};

class SyntaxViewState {
public:
    SyntaxViewState(Revision revision, LanguageId language,
                    std::uint64_t textBytes, std::vector<SyntaxSpan> spans,
                    std::vector<SyntaxBracketPair> bracketPairs,
                    std::vector<UnmatchedBracket> unmatchedBrackets,
                    std::vector<CommentToken> commentTokens,
                    std::vector<CommentRange> commentRanges,
                    std::vector<LineIndentation> indentation);

    // The unhighlighted view for `text`: no spans or brackets, only the
    // indentation the caret and wrap logic always need.  The view a document
    // shows before (or without) a parse.
    [[nodiscard]] static SyntaxViewState plainText(
        Revision revision, LanguageId language, std::string_view text,
        std::uint32_t tabWidth);

    // The view derived from a parser's output: spans/brackets/comments as parsed,
    // falling back to plainText when the output did not parse.
    [[nodiscard]] static SyntaxViewState fromParse(
        Revision revision, LanguageId language, std::string_view text,
        const SyntaxParseOutput& output, const SyntaxConfig& config);

    // The offset of the bracket matching the one at `offset`, or nullopt when
    // `offset` is not on a matched bracket.
    [[nodiscard]] std::optional<ByteOffset> matchingBracket(
        ByteOffset offset) const;

    // The syntax scope covering `offset` (PlainText when none does).
    [[nodiscard]] SyntaxScope scopeAt(ByteOffset offset) const;

    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const LanguageId& language() const noexcept {
        return language_;
    }
    [[nodiscard]] std::uint64_t textBytes() const noexcept {
        return textBytes_;
    }
    [[nodiscard]] const std::vector<SyntaxSpan>& spans() const noexcept {
        return spans_;
    }
    [[nodiscard]] const std::vector<SyntaxBracketPair>& bracketPairs() const noexcept {
        return bracketPairs_;
    }
    [[nodiscard]] const std::vector<UnmatchedBracket>& unmatchedBrackets()
        const noexcept {
        return unmatchedBrackets_;
    }
    [[nodiscard]] const std::vector<CommentToken>& commentTokens()
        const noexcept {
        return commentTokens_;
    }
    [[nodiscard]] const std::vector<CommentRange>& commentRanges()
        const noexcept {
        return commentRanges_;
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
    std::uint64_t textBytes_;
    std::vector<SyntaxSpan> spans_;
    std::vector<SyntaxBracketPair> bracketPairs_;
    std::vector<UnmatchedBracket> unmatchedBrackets_;
    std::vector<CommentToken> commentTokens_;
    std::vector<CommentRange> commentRanges_;
    std::vector<LineIndentation> indentation_;
};

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
    bool usedFallback = false;

    [[nodiscard]] bool accepted() const noexcept {
        return error == SyntaxAcceptError::None;
    }
};

// The outcome of the synchronous SyntaxModel::parse convenience: the request
// refusal (if any), the accept outcome (usually None on one thread, but a
// self-cancelling parser yields Cancelled and a mismatched-revision output
// yields MalformedOutput), and whether the parse fell back to plain text.
struct SyntaxParseResult {
    SyntaxRequestError requestError = SyntaxRequestError::None;
    SyntaxAcceptError acceptError = SyntaxAcceptError::None;
    bool usedFallback = false;

    [[nodiscard]] bool accepted() const noexcept {
        return requestError == SyntaxRequestError::None &&
               acceptError == SyntaxAcceptError::None;
    }
};

class SyntaxModel {
public:
    explicit SyntaxModel(std::shared_ptr<SyntaxParser> parser = nullptr,
                         SyntaxConfig config = {});

    [[nodiscard]] bool hasParser() const noexcept {
        return parser_ != nullptr;
    }
    [[nodiscard]] bool hasGrammar(const LanguageId& language) const noexcept;
    [[nodiscard]] bool canIncrementallyParse(
        const LanguageId& language) const noexcept;

    // Parse `text` and adopt the result, driving request -> run -> accept inline
    // on the CALLING thread. The convenience for the common synchronous case: it
    // cannot be called in the wrong order and needs no separate cancel. The
    // three-call API below remains for the off-thread seam, where run() executes
    // on a worker while the model is used on the main thread.
    [[nodiscard]] SyntaxParseResult parse(
        Revision revision, LanguageId language, std::string text,
        std::vector<SyntaxEdit> edits = {});

    [[nodiscard]] SyntaxParseRequestResult request(
        Revision revision, LanguageId language, std::string text,
        std::vector<SyntaxEdit> edits = {});
    [[nodiscard]] SyntaxParseOutput run(
        const SyntaxParseRequest& request) const;
    [[nodiscard]] SyntaxAcceptResult accept(
        const std::shared_ptr<const SyntaxParseRequest>& request,
        const SyntaxParseOutput& output);
    void cancelPending() noexcept;

    [[nodiscard]] const SyntaxViewState& viewState() const noexcept {
        return viewState_;
    }

private:
    std::shared_ptr<SyntaxParser> parser_;
    SyntaxConfig config_;
    SyntaxViewState viewState_;
    SyntaxParseHandle acceptedParse_;
    std::string acceptedText_;
    std::shared_ptr<const SyntaxParseRequest> pending_;
};

} // namespace ssg
