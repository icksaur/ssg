#include <ssg/SyntaxModel.h>

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

constexpr std::array<LanguageExtension, 17> kLanguageExtensions{{
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
    {".md", "markdown"},
    {".markdown", "markdown"},
    {".mdown", "markdown"},
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

template <typename T>
std::optional<T> changed(const T& before, const T& after) {
    if (before == after) {
        return std::nullopt;
    }
    return after;
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
    std::uint64_t revision, LanguageId language, std::string text,
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
    std::uint64_t revision, LanguageId language, std::uint64_t textBytes,
    std::vector<SyntaxSpan> spans)
    : revision_(revision),
      language_(std::move(language)),
      textBytes_(textBytes),
      spans_(std::move(spans)) {}

SyntaxViewState SyntaxViewState::plainText(
    std::uint64_t revision, LanguageId language, std::string_view text,
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
            std::move(spans)};
}

SyntaxViewState SyntaxViewState::fromParse(
    std::uint64_t revision, LanguageId language, std::string_view text,
    const SyntaxParseOutput& output, const SyntaxConfig& config) {
    if (config.tabWidth == 0) {
        throw std::invalid_argument{"tab width must be positive"};
    }
    if (output.status != SyntaxParseStatus::Parsed || !output.parse) {
        return SyntaxViewState::plainText(
            revision, std::move(language), text, config.tabWidth);
    }
    return {
        revision,
        std::move(language),
        text.size(),
        canonicalSpans(text.size(), output.spans),
    };
}

SyntaxScope SyntaxViewState::scopeAt(ByteOffset offset) const {
    if (offset.value() >= textBytes()) {
        return SyntaxScope::PlainText;
    }
    const auto span = std::upper_bound(
        spans().begin(), spans().end(), offset,
        [](ByteOffset position, const SyntaxSpan& candidate) {
            return position < candidate.begin;
        });
    if (span == spans().begin()) {
        return SyntaxScope::PlainText;
    }
    return std::prev(span)->scope;
}

SyntaxModel::SyntaxModel(std::shared_ptr<SyntaxParser> parser,
                         SyntaxConfig config)
    : parser_(std::move(parser)),
      config_(config),
      viewState_(std::make_shared<const SyntaxViewState>(
          SyntaxViewState::plainText(
              std::uint64_t{0}, LanguageId::plainText(), {},
              config.tabWidth))) {
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

bool SyntaxModel::canIncrementallyParse(
    const LanguageId& language) const noexcept {
    return acceptedParse_ != nullptr && language == viewState().language();
}

SyntaxParseResult SyntaxModel::parse(
    std::uint64_t revision, LanguageId language, std::string text,
    std::vector<SyntaxEdit> edits) {
    auto prepared = request(revision, std::move(language), std::move(text),
                            std::move(edits));
    if (!prepared.accepted()) {
        return {prepared.error, SyntaxAcceptError::None, false};
    }
    auto const output = run(*prepared.request);
    auto const accepted = accept(prepared.request, output);
    return {SyntaxRequestError::None, accepted.error, accepted.usedFallback};
}

SyntaxParseRequestResult SyntaxModel::request(
    std::uint64_t revision, LanguageId language, std::string text,
    std::vector<SyntaxEdit> edits) {
    const bool repeatsView =
        revision == viewState().revision() &&
        language == viewState().language();
    const bool repeatsPending =
        pending_ && revision == pending_->revision() &&
        language == pending_->language();
    if (revision < viewState().revision() || repeatsView ||
        (pending_ && revision < pending_->revision()) || repeatsPending) {
        return {nullptr, SyntaxRequestError::StaleRevision};
    }
    cancelPending();
    if (text.size() > config_.maximumDocumentBytes) {
        viewState_ = std::make_shared<const SyntaxViewState>(
            SyntaxViewState::plainText(
                revision, language, text, config_.tabWidth));
        acceptedParse_.reset();
        acceptedText_ = text;
        pending_.reset();
        return {nullptr, SyntaxRequestError::DocumentTooLarge};
    }
    const auto priorParse =
        language == viewState().language() ? acceptedParse_ : nullptr;
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
    return runSyntaxParse(parser_, request);
}

SyntaxParseOutput runSyntaxParse(
    const std::shared_ptr<SyntaxParser>& parser,
    const SyntaxParseRequest& request) {
    if (request.cancelled()) {
        return {.revision = request.revision(),
                .status = SyntaxParseStatus::Cancelled};
    }
    bool grammarAvailable = false;
    try {
        grammarAvailable =
            parser && !request.language().isPlainText() &&
            parser->hasGrammar(request.language());
    } catch (...) {
    }
    if (!grammarAvailable) {
        return {.revision = request.revision(),
                .status = SyntaxParseStatus::GrammarUnavailable};
    }
    try {
        return parser->parse(request);
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
    if (request->revision() < viewState().revision() ||
        (request->revision() == viewState().revision() &&
         request->language() == viewState().language())) {
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
                    ? SyntaxViewState::plainText(
                          request->revision(), request->language(),
                          request->text(), config_.tabWidth)
                    : SyntaxViewState::fromParse(
                          request->revision(), request->language(),
                          request->text(), output, config_);
    viewState_ =
        std::make_shared<const SyntaxViewState>(std::move(next));
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
