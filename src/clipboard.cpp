#include <ssg/clipboard.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

struct ByteRange {
    std::uint64_t begin;
    std::uint64_t end;
};

struct RegisterData {
    std::vector<std::string> fragments;
    std::string plainText;
};

struct PendingRead {
    ClipboardRequest request;
    SelectionSet selections;
    RegisterData fallback;
};

ClipboardResult failure(ClipboardError error, Revision revision,
                        ClipboardSystemStatus status, std::string message) {
    return {error, status, revision, std::nullopt, std::nullopt, false,
            std::move(message)};
}

ClipboardSystemStatus systemStatus(ClipboardResponseStatus status) {
    switch (status) {
        case ClipboardResponseStatus::Success:
            return ClipboardSystemStatus::Succeeded;
        case ClipboardResponseStatus::Denied:
            return ClipboardSystemStatus::Denied;
        case ClipboardResponseStatus::Unavailable:
            return ClipboardSystemStatus::Unavailable;
        case ClipboardResponseStatus::Disconnected:
            return ClipboardSystemStatus::Disconnected;
    }
    return ClipboardSystemStatus::Unavailable;
}

bool positionIsValid(std::string_view text, const DocumentPosition& position,
                       int tabWidth) {
    const auto resolved =
        resolveDocumentPosition(text, position.byteOffset, tabWidth);
    return resolved.has_value() && *resolved == position;
}

bool selectionsAreValid(std::string_view text,
                          const SelectionSet& selections, int tabWidth) {
    return std::all_of(
        selections.items().begin(), selections.items().end(),
        [&](const Selection& selection) {
            return positionIsValid(text, selection.anchor, tabWidth) &&
                   positionIsValid(text, selection.active, tabWidth);
        });
}

ByteRange lineRange(std::string_view text, std::uint64_t offset) {
    std::uint64_t begin = offset;
    while (begin > 0) {
        const auto previous = text[static_cast<std::size_t>(begin - 1)];
        if (previous == '\n' || previous == '\r') {
            break;
        }
        --begin;
    }

    std::uint64_t end = offset;
    while (end < text.size() && text[static_cast<std::size_t>(end)] != '\n' &&
           text[static_cast<std::size_t>(end)] != '\r') {
        ++end;
    }
    if (end < text.size()) {
        if (text[static_cast<std::size_t>(end)] == '\r' &&
            end + 1 < text.size() &&
            text[static_cast<std::size_t>(end + 1)] == '\n') {
            end += 2;
        } else {
            ++end;
        }
    }
    return {begin, end};
}

ByteRange selectionRange(std::string_view text,
                          const Selection& selection) {
    if (selection.isCaret()) {
        return lineRange(text, selection.active.byteOffset.value());
    }
    return {selection.lower().byteOffset.value(),
            selection.upper().byteOffset.value()};
}

RegisterData capture(std::string_view text, const SelectionSet& selections) {
    RegisterData result;
    result.fragments.reserve(selections.items().size());
    for (const auto& selection : selections.items()) {
        const auto range = selectionRange(text, selection);
        auto fragment = std::string{text.substr(
            static_cast<std::size_t>(range.begin),
            static_cast<std::size_t>(range.end - range.begin))};
        result.plainText += fragment;
        result.fragments.push_back(std::move(fragment));
    }
    return result;
}

std::vector<ByteRange> mergedCutRanges(std::string_view text,
                                         const SelectionSet& selections) {
    std::vector<ByteRange> ranges;
    ranges.reserve(selections.items().size());
    for (const auto& selection : selections.items()) {
        const auto range = selectionRange(text, selection);
        if (range.begin != range.end) {
            ranges.push_back(range);
        }
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const ByteRange& left, const ByteRange& right) {
                  return left.begin < right.begin ||
                         (left.begin == right.begin && left.end < right.end);
              });
    std::vector<ByteRange> merged;
    for (const auto range : ranges) {
        if (!merged.empty() && range.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, range.end);
        } else {
            merged.push_back(range);
        }
    }
    return merged;
}

std::string applyReplacements(
    std::string text, const std::vector<TextEdit>& edits) {
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        text.replace(static_cast<std::size_t>(it->offset.value()),
                     static_cast<std::size_t>(it->erasedBytes),
                     it->insertedText);
    }
    return text;
}

std::optional<SelectionSet> cutSelections(
    std::string_view resultingText, const std::vector<ByteRange>& ranges,
    int tabWidth) {
    std::vector<Selection> selections;
    selections.reserve(ranges.size());
    std::uint64_t erasedBefore = 0;
    for (const auto range : ranges) {
        const auto offset = range.begin - erasedBefore;
        const auto position = resolveDocumentPosition(
            resultingText, ByteOffset{offset}, tabWidth);
        if (!position) {
            return std::nullopt;
        }
        selections.push_back({*position, *position});
        erasedBefore += range.end - range.begin;
    }
    return SelectionSet{std::move(selections)};
}

std::optional<SelectionSet> pasteSelections(
    std::string_view resultingText, const SelectionSet& before,
    const std::vector<std::string>& insertions, int tabWidth) {
    std::vector<Selection> selections;
    selections.reserve(before.items().size());
    std::int64_t deltaBefore = 0;
    for (std::size_t index = 0; index < before.items().size(); ++index) {
        const auto& selection = before.items()[index];
        const auto begin = selection.lower().byteOffset.value();
        const auto erased =
            selection.upper().byteOffset.value() - begin;
        const auto signedOffset =
            static_cast<std::int64_t>(begin) + deltaBefore +
            static_cast<std::int64_t>(insertions[index].size());
        if (signedOffset < 0) {
            return std::nullopt;
        }
        const auto position = resolveDocumentPosition(
            resultingText, ByteOffset{static_cast<std::uint64_t>(signedOffset)},
            tabWidth);
        if (!position) {
            return std::nullopt;
        }
        selections.push_back({*position, *position});
        deltaBefore += static_cast<std::int64_t>(insertions[index].size()) -
                        static_cast<std::int64_t>(erased);
    }
    return SelectionSet{std::move(selections)};
}

ClipboardError modeError(DocumentMode mode) {
    if (mode == DocumentMode::ReadOnly) {
        return ClipboardError::ReadOnly;
    }
    if (mode == DocumentMode::Diff) {
        return ClipboardError::Diff;
    }
    return ClipboardError::None;
}

std::string modeMessage(DocumentMode mode) {
    return mode == DocumentMode::ReadOnly
               ? "clipboard mutation rejected: document is read-only"
               : "clipboard mutation rejected: document is a diff";
}

ClipboardError documentError(DocumentError error) {
    if (error == DocumentError::InvalidUtf8) {
        return ClipboardError::InvalidUtf8;
    }
    if (error == DocumentError::ReadOnly) {
        return ClipboardError::ReadOnly;
    }
    if (error == DocumentError::Diff) {
        return ClipboardError::Diff;
    }
    return ClipboardError::DocumentRejected;
}

}  // namespace

struct ClipboardRegister::Impl {
    explicit Impl(int width) : tabWidth(width) {}

    int tabWidth;
    std::uint64_t nextRequestId = 1;
    RegisterData registerData;
    std::optional<ClipboardRequest> pendingWrite;
    std::optional<PendingRead> pendingRead;

    std::optional<ClipboardRequest> request(ClipboardRequestKind kind,
                                            Revision revision,
                                            std::string text) {
        if (nextRequestId == std::numeric_limits<std::uint64_t>::max()) {
            return std::nullopt;
        }
        return ClipboardRequest{nextRequestId++, kind, revision,
                                std::move(text)};
    }

    ClipboardResult pasteData(Document& document, DocumentHistory& history,
                               const SelectionSet& selections,
                               const RegisterData& data,
                               std::uint64_t timestampMs,
                               ClipboardSystemStatus status) {
        const auto snapshot = document.snapshot();
        if (const auto error = modeError(snapshot.mode);
            error != ClipboardError::None) {
            return failure(error, snapshot.revision, status,
                           modeMessage(snapshot.mode));
        }
        if (!selectionsAreValid(snapshot.text, selections, tabWidth)) {
            return failure(ClipboardError::InvalidSelection, snapshot.revision,
                           status, "clipboard paste has an invalid selection");
        }
        if (data.fragments.empty()) {
            return {ClipboardError::None, status, snapshot.revision, selections,
                    std::nullopt, false, {}};
        }

        std::vector<std::string> insertions;
        insertions.reserve(selections.items().size());
        const auto distribute =
            data.fragments.size() == selections.items().size();
        for (std::size_t index = 0; index < selections.items().size(); ++index) {
            insertions.push_back(distribute ? data.fragments[index]
                                            : data.plainText);
        }

        std::vector<TextEdit> edits;
        edits.reserve(selections.items().size());
        for (std::size_t index = 0; index < selections.items().size(); ++index) {
            const auto& selection = selections.items()[index];
            const auto begin = selection.lower().byteOffset.value();
            edits.push_back(
                {ByteOffset{begin},
                 selection.upper().byteOffset.value() - begin,
                 insertions[index]});
        }
        const auto resultingText =
            applyReplacements(snapshot.text, edits);
        if (resultingText == snapshot.text) {
            return {ClipboardError::None, status, snapshot.revision, selections,
                    std::nullopt, false, {}};
        }
        const auto after = pasteSelections(resultingText, selections,
                                            insertions, tabWidth);
        if (!after) {
            return failure(ClipboardError::InvalidUtf8, snapshot.revision,
                           status,
                           "clipboard text is not well-formed UTF-8");
        }

        const auto historyResult = history.applyEdit(
            document, EditTransaction{snapshot.revision, std::move(edits)},
            selections, *after, HistoryEditKind::Other, timestampMs);
        if (!historyResult.accepted()) {
            return failure(documentError(historyResult.documentError),
                           document.revision(), status, historyResult.message);
        }
        return {ClipboardError::None, status, historyResult.revision,
                historyResult.selections, std::nullopt, true, {}};
    }
};

ClipboardCommandSet::ClipboardCommandSet()
    : descriptors_{{{"clipboard.copy", ClipboardCommand::Copy},
                    {"clipboard.cut", ClipboardCommand::Cut},
                    {"clipboard.paste", ClipboardCommand::Paste}}} {}

const std::array<ClipboardCommandDescriptor, 3>&
ClipboardCommandSet::descriptors() const noexcept {
    return descriptors_;
}

ClipboardCommandSet clipboardCommandSet() {
    return ClipboardCommandSet{};
}

ClipboardDelta deriveClipboardDelta(const ClipboardViewState& before,
                                      const ClipboardViewState& after) {
    if (before == after) {
        return {false, std::nullopt};
    }
    return {true, after};
}

ClipboardRegister::ClipboardRegister(int tabWidth)
    : impl_(std::make_unique<Impl>(tabWidth)) {
    if (tabWidth < 1 || tabWidth > 16) {
        throw std::invalid_argument(
            "clipboard tab width must be in the inclusive range [1, 16]");
    }
}

ClipboardRegister::~ClipboardRegister() = default;
ClipboardRegister::ClipboardRegister(ClipboardRegister&&) noexcept = default;
ClipboardRegister& ClipboardRegister::operator=(ClipboardRegister&&) noexcept =
    default;

ClipboardResult ClipboardRegister::copy(const DocumentSnapshot& document,
                                        const SelectionSet& selections) {
    if (!selectionsAreValid(document.text, selections, impl_->tabWidth)) {
        return failure(ClipboardError::InvalidSelection, document.revision,
                       ClipboardSystemStatus::NotRequested,
                       "clipboard copy has an invalid selection");
    }
    auto captured = capture(document.text, selections);
    auto request = impl_->request(ClipboardRequestKind::Write,
                                  document.revision, captured.plainText);
    if (!request) {
        return failure(ClipboardError::RequestExhausted, document.revision,
                       ClipboardSystemStatus::NotRequested,
                       "clipboard request identifiers are exhausted");
    }
    impl_->registerData = std::move(captured);
    impl_->pendingWrite = *request;
    return {ClipboardError::None, ClipboardSystemStatus::Pending,
            document.revision, selections, std::move(request), false, {}};
}

ClipboardResult ClipboardRegister::cut(Document& document,
                                       DocumentHistory& history,
                                       const SelectionSet& selections,
                                       std::uint64_t timestampMs) {
    const auto snapshot = document.snapshot();
    if (const auto error = modeError(snapshot.mode);
        error != ClipboardError::None) {
        return failure(error, snapshot.revision,
                       ClipboardSystemStatus::NotRequested,
                       modeMessage(snapshot.mode));
    }
    if (!selectionsAreValid(snapshot.text, selections, impl_->tabWidth)) {
        return failure(ClipboardError::InvalidSelection, snapshot.revision,
                       ClipboardSystemStatus::NotRequested,
                       "clipboard cut has an invalid selection");
    }

    auto captured = capture(snapshot.text, selections);
    const auto ranges = mergedCutRanges(snapshot.text, selections);
    auto resultingText = snapshot.text;
    std::optional<SelectionSet> after = selections;
    bool changed = !ranges.empty();
    Revision revision = snapshot.revision;

    if (changed) {
        std::vector<TextEdit> edits;
        edits.reserve(ranges.size());
        for (const auto range : ranges) {
            edits.push_back(
                {ByteOffset{range.begin}, range.end - range.begin, {}});
        }
        resultingText = applyReplacements(snapshot.text, edits);
        after = cutSelections(resultingText, ranges, impl_->tabWidth);
        if (!after) {
            return failure(ClipboardError::DocumentRejected,
                           snapshot.revision,
                           ClipboardSystemStatus::NotRequested,
                           "clipboard cut could not resolve its selections");
        }
        const auto historyResult = history.applyEdit(
            document, EditTransaction{snapshot.revision, std::move(edits)},
            selections, *after, HistoryEditKind::Other, timestampMs);
        if (!historyResult.accepted()) {
            return failure(documentError(historyResult.documentError),
                           document.revision(),
                           ClipboardSystemStatus::NotRequested,
                           historyResult.message);
        }
        revision = historyResult.revision;
        after = historyResult.selections;
    }

    auto request = impl_->request(ClipboardRequestKind::Write, revision,
                                  captured.plainText);
    if (!request) {
        return failure(ClipboardError::RequestExhausted, revision,
                       ClipboardSystemStatus::NotRequested,
                       "clipboard request identifiers are exhausted");
    }
    impl_->registerData = std::move(captured);
    impl_->pendingWrite = *request;
    return {ClipboardError::None, ClipboardSystemStatus::Pending, revision,
            std::move(after), std::move(request), changed, {}};
}

ClipboardResult ClipboardRegister::paste(Document& document,
                                         DocumentHistory& history,
                                         const SelectionSet& selections,
                                         ClipboardPasteMode mode,
                                         std::uint64_t timestampMs) {
    const auto snapshot = document.snapshot();
    if (const auto error = modeError(snapshot.mode);
        error != ClipboardError::None) {
        return failure(error, snapshot.revision,
                       ClipboardSystemStatus::NotRequested,
                       modeMessage(snapshot.mode));
    }
    if (!selectionsAreValid(snapshot.text, selections, impl_->tabWidth)) {
        return failure(ClipboardError::InvalidSelection, snapshot.revision,
                       ClipboardSystemStatus::NotRequested,
                       "clipboard paste has an invalid selection");
    }
    if (mode == ClipboardPasteMode::InternalOnly) {
        return impl_->pasteData(document, history, selections,
                                 impl_->registerData, timestampMs,
                                 ClipboardSystemStatus::NotRequested);
    }

    auto request =
        impl_->request(ClipboardRequestKind::Read, snapshot.revision, {});
    if (!request) {
        return failure(ClipboardError::RequestExhausted, snapshot.revision,
                       ClipboardSystemStatus::NotRequested,
                       "clipboard request identifiers are exhausted");
    }
    impl_->pendingRead =
        PendingRead{*request, selections, impl_->registerData};
    return {ClipboardError::None, ClipboardSystemStatus::Pending,
            snapshot.revision, selections, std::move(request), false, {}};
}

ClipboardResult ClipboardRegister::handleResponse(
    Document& document, DocumentHistory& history,
    const SelectionSet& currentSelections, const ClipboardResponse& response,
    std::uint64_t timestampMs) {
    const auto currentRevision = document.revision();
    if (impl_->pendingWrite && impl_->pendingWrite->id == response.id) {
        const auto request = *impl_->pendingWrite;
        impl_->pendingWrite.reset();
        if (response.requestRevision != request.requestRevision ||
            response.observedDocumentRevision != request.requestRevision) {
            return failure(ClipboardError::StaleResponse, currentRevision,
                           ClipboardSystemStatus::Stale,
                           "stale clipboard write response");
        }
        return {ClipboardError::None, systemStatus(response.status),
                currentRevision, currentSelections, std::nullopt, false, {}};
    }

    if (!impl_->pendingRead || impl_->pendingRead->request.id != response.id) {
        return failure(ClipboardError::NoRequest, currentRevision,
                       ClipboardSystemStatus::Stale,
                       "clipboard response does not match an outstanding request");
    }
    auto pending = std::move(*impl_->pendingRead);
    impl_->pendingRead.reset();
    if (response.requestRevision != pending.request.requestRevision ||
        response.observedDocumentRevision !=
            pending.request.requestRevision ||
        currentRevision != pending.request.requestRevision ||
        currentSelections != pending.selections) {
        return failure(ClipboardError::StaleResponse, currentRevision,
                       ClipboardSystemStatus::Stale,
                       "stale clipboard read response");
    }

    const auto status = systemStatus(response.status);
    if (response.status == ClipboardResponseStatus::Success) {
        RegisterData systemData{{response.text}, response.text};
        return impl_->pasteData(document, history, currentSelections,
                                 systemData, timestampMs, status);
    }
    return impl_->pasteData(document, history, currentSelections,
                             pending.fallback, timestampMs, status);
}

ClipboardViewState ClipboardRegister::viewState() const {
    std::optional<ClipboardRequest> read;
    if (impl_->pendingRead) {
        read = impl_->pendingRead->request;
    }
    return {impl_->registerData.fragments, impl_->registerData.plainText,
            std::move(read), impl_->pendingWrite};
}

}  // namespace ssg
