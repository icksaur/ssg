#include <ssg/history.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ssg {
namespace {

struct HistoryStep {
    std::vector<TextEdit> forward;
    std::vector<TextEdit> inverse;
    std::uint64_t payloadBytes;
};

struct HistoryUnit {
    std::vector<HistoryStep> steps;
    SelectionSet selectionsBefore;
    SelectionSet selectionsAfter;
    HistoryEditKind kind;
    std::uint64_t lastTimestampMs;
    std::uint64_t chargedBytes;
    bool coalescible;
};

HistoryResult failure(HistoryError error, Revision revision,
                      std::string message,
                      DocumentError documentError = DocumentError::None) {
    return {error, documentError, revision, std::nullopt, std::move(message)};
}

std::uint64_t selectionCharge(const SelectionSet& selections) {
    const auto count = static_cast<std::uint64_t>(selections.items().size());
    constexpr auto itemSize = static_cast<std::uint64_t>(sizeof(Selection));
    if (count > std::numeric_limits<std::uint64_t>::max() / itemSize) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return count * itemSize;
}

std::uint64_t saturatedAdd(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

bool allCarets(const SelectionSet& selections) {
    return std::all_of(selections.items().begin(), selections.items().end(),
                       [](const Selection& selection) {
                           return selection.isCaret();
                       });
}

std::vector<const TextEdit*> orderedEdits(
    const std::vector<TextEdit>& edits) {
    std::vector<const TextEdit*> ordered;
    ordered.reserve(edits.size());
    for (const auto& edit : edits) {
        ordered.push_back(&edit);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const TextEdit* left, const TextEdit* right) {
                  return left->offset < right->offset;
              });
    return ordered;
}

bool editShapeMatches(const EditTransaction& transaction,
                        const SelectionSet& selections,
                        HistoryEditKind kind) {
    if (kind == HistoryEditKind::Other || !allCarets(selections) ||
        transaction.edits.size() != selections.items().size()) {
        return false;
    }
    const auto ordered = orderedEdits(transaction.edits);
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const auto& edit = *ordered[index];
        const auto caret =
            selections.items()[index].active.byteOffset.value();
        switch (kind) {
        case HistoryEditKind::Typing:
            if (edit.erasedBytes != 0 || edit.insertedText.empty() ||
                edit.offset.value() != caret) {
                return false;
            }
            break;
        case HistoryEditKind::DeleteBackward:
            if (!edit.insertedText.empty() || edit.erasedBytes == 0 ||
                edit.offset.value() + edit.erasedBytes != caret) {
                return false;
            }
            break;
        case HistoryEditKind::DeleteForward:
            if (!edit.insertedText.empty() || edit.erasedBytes == 0 ||
                edit.offset.value() != caret) {
                return false;
            }
            break;
        case HistoryEditKind::Other:
            return false;
        }
    }
    return true;
}

bool rangesAreCapturable(const DocumentSnapshot& before,
                           const EditTransaction& transaction) {
    for (const auto& edit : transaction.edits) {
        if (edit.offset.value() > before.text.size() ||
            edit.erasedBytes >
                before.text.size() -
                    static_cast<std::size_t>(edit.offset.value())) {
            return false;
        }
    }
    return true;
}

HistoryStep makeStep(const DocumentSnapshot& before,
                      const EditTransaction& transaction) {
    HistoryStep step{{}, {}, 0};
    step.forward = transaction.edits;
    const auto ordered = orderedEdits(transaction.edits);
    std::int64_t displacement = 0;
    step.inverse.reserve(ordered.size());
    for (const auto* edit : ordered) {
        const auto offset = static_cast<std::int64_t>(edit->offset.value());
        const auto outputOffset = offset + displacement;
        const auto erased = before.text.substr(
            static_cast<std::size_t>(edit->offset.value()),
            static_cast<std::size_t>(edit->erasedBytes));
        step.inverse.push_back(
            {ByteOffset{static_cast<std::uint64_t>(outputOffset)},
             static_cast<std::uint64_t>(edit->insertedText.size()), erased});
        step.payloadBytes = saturatedAdd(
            step.payloadBytes,
            saturatedAdd(static_cast<std::uint64_t>(edit->insertedText.size()),
                          edit->erasedBytes));
        displacement +=
            static_cast<std::int64_t>(edit->insertedText.size()) -
            static_cast<std::int64_t>(edit->erasedBytes);
    }
    return step;
}

std::uint64_t unitCharge(const HistoryStep& step,
                          const SelectionSet& before,
                          const SelectionSet& after) {
    return saturatedAdd(
        step.payloadBytes,
        saturatedAdd(selectionCharge(before), selectionCharge(after)));
}

}  // namespace

struct DocumentHistory::Impl {
    explicit Impl(HistoryConfig historyConfig) : config(historyConfig) {}

    HistoryConfig config;
    std::vector<HistoryUnit> undo;
    std::vector<HistoryUnit> redo;
    std::optional<Revision> expectedRevision;
    std::uint64_t retained{0};
    bool barrier{true};

    void removeCharge(const HistoryUnit& unit) noexcept {
        retained -= unit.chargedBytes;
    }

    void clearRedo() noexcept {
        for (const auto& unit : redo) {
            removeCharge(unit);
        }
        redo.clear();
    }

    void clearAll() noexcept {
        undo.clear();
        redo.clear();
        retained = 0;
    }

    void enforceBudget() {
        while (retained > config.byteBudget && !undo.empty()) {
            removeCharge(undo.front());
            undo.erase(undo.begin());
        }
    }

    bool canCoalesce(const HistoryUnit& unit, const HistoryStep& step,
                      const SelectionSet& before, HistoryEditKind kind,
                      std::uint64_t timestampMs,
                      bool stepCoalescible) const {
        (void)step;
        return !barrier && unit.coalescible && stepCoalescible &&
               unit.kind == kind &&
               unit.selectionsAfter == before &&
               timestampMs >= unit.lastTimestampMs &&
               timestampMs - unit.lastTimestampMs <= config.coalesceMs;
    }
};

HistoryCommandSet::HistoryCommandSet()
    : descriptors_{{{"edit.undo", HistoryCommand::Undo},
                    {"edit.redo", HistoryCommand::Redo}}} {}

const std::array<HistoryCommandDescriptor, 2>&
HistoryCommandSet::descriptors() const noexcept {
    return descriptors_;
}

HistoryCommandSet historyCommandSet() {
    return HistoryCommandSet{};
}

HistoryDelta HistoryDeltaCodec::derive(const HistoryViewState& before,
                                  const HistoryViewState& after) {
    if (before == after) {
        return {false, std::nullopt};
    }
    return {true, after};
}

DocumentHistory::DocumentHistory(HistoryConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

DocumentHistory::~DocumentHistory() = default;
DocumentHistory::DocumentHistory(DocumentHistory&&) noexcept = default;
DocumentHistory& DocumentHistory::operator=(DocumentHistory&&) noexcept =
    default;

HistoryResult DocumentHistory::applyEdit(
    Document& document, const EditTransaction& transaction,
    const SelectionSet& selectionsBefore,
    const SelectionSet& selectionsAfter, HistoryEditKind kind,
    std::uint64_t timestampMs) {
    const auto before = document.snapshot();
    const bool bypassed =
        impl_->expectedRevision &&
        *impl_->expectedRevision != before.revision;
    if (!rangesAreCapturable(before, transaction)) {
        const auto result = document.apply(transaction);
        return failure(HistoryError::DocumentRejected, result.revision,
                       result.message, result.error);
    }

    auto step = makeStep(before, transaction);
    const auto result = document.apply(transaction);
    if (!result.accepted()) {
        return failure(HistoryError::DocumentRejected, result.revision,
                       result.message, result.error);
    }

    if (bypassed) {
        impl_->clearAll();
        impl_->barrier = true;
    }
    impl_->clearRedo();

    const bool stepCoalescible =
        editShapeMatches(transaction, selectionsBefore, kind);
    if (!impl_->undo.empty() &&
        impl_->canCoalesce(impl_->undo.back(), step, selectionsBefore, kind,
                            timestampMs, stepCoalescible)) {
        auto& unit = impl_->undo.back();
        impl_->retained -= unit.chargedBytes;
        unit.steps.push_back(std::move(step));
        unit.selectionsAfter = selectionsAfter;
        unit.lastTimestampMs = timestampMs;
        unit.chargedBytes = saturatedAdd(
            unit.chargedBytes,
            unit.steps.back().payloadBytes);
        impl_->retained =
            saturatedAdd(impl_->retained, unit.chargedBytes);
    } else {
        const auto charge =
            unitCharge(step, selectionsBefore, selectionsAfter);
        impl_->undo.push_back(
            {{std::move(step)}, selectionsBefore, selectionsAfter, kind,
             timestampMs, charge, stepCoalescible});
        impl_->retained = saturatedAdd(impl_->retained, charge);
    }

    impl_->enforceBudget();
    impl_->expectedRevision = result.revision;
    impl_->barrier = false;
    return {HistoryError::None, DocumentError::None, result.revision,
            selectionsAfter, {}};
}

HistoryResult DocumentHistory::undo(Document& document) {
    if (impl_->undo.empty()) {
        return failure(HistoryError::NoUndo, document.revision(),
                       "document has no undo history");
    }
    if (!impl_->expectedRevision ||
        document.revision() != *impl_->expectedRevision) {
        return failure(HistoryError::StaleDocument, document.revision(),
                       "document revision changed outside its history");
    }

    auto& unit = impl_->undo.back();
    const auto revision = document.revision().value();
    if (unit.steps.size() >
        std::numeric_limits<std::uint64_t>::max() - revision) {
        return failure(HistoryError::RevisionExhausted, document.revision(),
                       "undo would exhaust the document revision");
    }

    for (auto iterator = unit.steps.rbegin(); iterator != unit.steps.rend();
         ++iterator) {
        const auto result =
            document.apply({document.revision(), iterator->inverse});
        if (!result.accepted()) {
            return failure(HistoryError::DocumentRejected, result.revision,
                           result.message, result.error);
        }
    }

    auto moved = std::move(impl_->undo.back());
    impl_->undo.pop_back();
    const auto selections = moved.selectionsBefore;
    impl_->redo.push_back(std::move(moved));
    impl_->expectedRevision = document.revision();
    impl_->barrier = true;
    return {HistoryError::None, DocumentError::None, document.revision(),
            selections, {}};
}

HistoryResult DocumentHistory::redo(Document& document) {
    if (impl_->redo.empty()) {
        return failure(HistoryError::NoRedo, document.revision(),
                       "document has no redo history");
    }
    if (!impl_->expectedRevision ||
        document.revision() != *impl_->expectedRevision) {
        return failure(HistoryError::StaleDocument, document.revision(),
                       "document revision changed outside its history");
    }

    auto& unit = impl_->redo.back();
    const auto revision = document.revision().value();
    if (unit.steps.size() >
        std::numeric_limits<std::uint64_t>::max() - revision) {
        return failure(HistoryError::RevisionExhausted, document.revision(),
                       "redo would exhaust the document revision");
    }

    for (const auto& step : unit.steps) {
        const auto result =
            document.apply({document.revision(), step.forward});
        if (!result.accepted()) {
            return failure(HistoryError::DocumentRejected, result.revision,
                           result.message, result.error);
        }
    }

    auto moved = std::move(impl_->redo.back());
    impl_->redo.pop_back();
    const auto selections = moved.selectionsAfter;
    impl_->undo.push_back(std::move(moved));
    impl_->expectedRevision = document.revision();
    impl_->barrier = true;
    return {HistoryError::None, DocumentError::None, document.revision(),
            selections, {}};
}

void DocumentHistory::breakCoalescing() noexcept {
    impl_->barrier = true;
}

bool DocumentHistory::canUndo() const noexcept {
    return !impl_->undo.empty();
}

bool DocumentHistory::canRedo() const noexcept {
    return !impl_->redo.empty();
}

std::uint64_t DocumentHistory::retainedBytes() const noexcept {
    return impl_->retained;
}

HistoryViewState DocumentHistory::viewState() const noexcept {
    return {canUndo(), canRedo(), retainedBytes()};
}

}  // namespace ssg
