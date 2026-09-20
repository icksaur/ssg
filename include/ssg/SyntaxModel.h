#pragma once

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

struct SyntaxSpan {
    ByteOffset begin;
    ByteOffset end;
    SyntaxScope scope = SyntaxScope::PlainText;

    friend bool operator==(const SyntaxSpan&, const SyntaxSpan&) = default;
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
    std::uint64_t revision{0};
    SyntaxParseStatus status = SyntaxParseStatus::Failed;
    SyntaxParseHandle parse;
    std::vector<SyntaxSpan> spans;
};

class SyntaxModel;

class SyntaxParseRequest {
public:
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
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

    SyntaxParseRequest(std::uint64_t revision, LanguageId language, std::string text,
                       SyntaxParseHandle priorParse,
                       std::vector<SyntaxEdit> edits);

    std::uint64_t revision_;
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

[[nodiscard]] SyntaxParseOutput runSyntaxParse(
    const std::shared_ptr<SyntaxParser>& parser,
    const SyntaxParseRequest& request);

struct SyntaxConfig {
    std::uint32_t tabWidth = 4;
    std::size_t maximumDocumentBytes = 64 * 1024 * 1024;
};

class SyntaxViewState {
public:
    SyntaxViewState(std::uint64_t revision, LanguageId language,
                    std::uint64_t textBytes, std::vector<SyntaxSpan> spans);

    // The unhighlighted view for `text`: no spans or brackets, only the
    // indentation the caret and wrap logic always need.  The view a document
    // shows before (or without) a parse.
    [[nodiscard]] static SyntaxViewState plainText(
        std::uint64_t revision, LanguageId language, std::string_view text,
        std::uint32_t tabWidth);

    // The view derived from a parser's output: spans/brackets/comments as parsed,
    // falling back to plainText when the output did not parse.
    [[nodiscard]] static SyntaxViewState fromParse(
        std::uint64_t revision, LanguageId language, std::string_view text,
        const SyntaxParseOutput& output, const SyntaxConfig& config);

    // The syntax scope covering `offset` (PlainText when none does).
    [[nodiscard]] SyntaxScope scopeAt(ByteOffset offset) const;

    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const LanguageId& language() const noexcept {
        return language_;
    }
    [[nodiscard]] std::uint64_t textBytes() const noexcept {
        return textBytes_;
    }
    [[nodiscard]] const std::vector<SyntaxSpan>& spans() const noexcept {
        return spans_;
    }
    friend bool operator==(const SyntaxViewState&,
                           const SyntaxViewState&) = default;

private:
    std::uint64_t revision_;
    LanguageId language_;
    std::uint64_t textBytes_;
    std::vector<SyntaxSpan> spans_;
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
    [[nodiscard]] bool hasPending() const noexcept {
        return pending_ != nullptr;
    }
    [[nodiscard]] std::shared_ptr<const SyntaxParseRequest> pendingRequest()
        const noexcept {
        return pending_;
    }

    // Parse `text` and adopt the result, driving request -> run -> accept inline
    // on the CALLING thread. The convenience for the common synchronous case: it
    // cannot be called in the wrong order and needs no separate cancel. The
    // three-call API below remains for the off-thread seam, where run() executes
    // on a worker while the model is used on the main thread.
    [[nodiscard]] SyntaxParseResult parse(
        std::uint64_t revision, LanguageId language, std::string text,
        std::vector<SyntaxEdit> edits = {});

    [[nodiscard]] SyntaxParseRequestResult request(
        std::uint64_t revision, LanguageId language, std::string text,
        std::vector<SyntaxEdit> edits = {});
    [[nodiscard]] SyntaxParseOutput run(
        const SyntaxParseRequest& request) const;
    [[nodiscard]] SyntaxAcceptResult accept(
        const std::shared_ptr<const SyntaxParseRequest>& request,
        const SyntaxParseOutput& output);
    void cancelPending() noexcept;

    [[nodiscard]] const SyntaxViewState& viewState() const noexcept {
        return *viewState_;
    }
    [[nodiscard]] std::shared_ptr<const SyntaxViewState> sharedViewState()
        const noexcept {
        return viewState_;
    }

private:
    std::shared_ptr<SyntaxParser> parser_;
    SyntaxConfig config_;
    std::shared_ptr<const SyntaxViewState> viewState_;
    SyntaxParseHandle acceptedParse_;
    std::string acceptedText_;
    std::shared_ptr<const SyntaxParseRequest> pending_;
};

} // namespace ssg
