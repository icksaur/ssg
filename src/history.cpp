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
    std::uint64_t payload_bytes;
};

struct HistoryUnit {
    std::vector<HistoryStep> steps;
    SelectionSet selections_before;
    SelectionSet selections_after;
    HistoryEditKind kind;
    std::uint64_t last_timestamp_ms;
    std::uint64_t charged_bytes;
    bool coalescible;
};

HistoryResult failure(HistoryError error, Revision revision,
                      std::string message,
                      DocumentError document_error = DocumentError::none) {
    return {error, document_error, revision, std::nullopt, std::move(message)};
}

std::uint64_t selection_charge(const SelectionSet& selections) {
    const auto count = static_cast<std::uint64_t>(selections.items().size());
    constexpr auto item_size = static_cast<std::uint64_t>(sizeof(Selection));
    if (count > std::numeric_limits<std::uint64_t>::max() / item_size) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return count * item_size;
}

std::uint64_t saturated_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

bool all_carets(const SelectionSet& selections) {
    return std::all_of(selections.items().begin(), selections.items().end(),
                       [](const Selection& selection) {
                           return selection.is_caret();
                       });
}

std::vector<const TextEdit*> ordered_edits(
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

bool edit_shape_matches(const EditTransaction& transaction,
                        const SelectionSet& selections,
                        HistoryEditKind kind) {
    if (kind == HistoryEditKind::other || !all_carets(selections) ||
        transaction.edits.size() != selections.items().size()) {
        return false;
    }
    const auto ordered = ordered_edits(transaction.edits);
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const auto& edit = *ordered[index];
        const auto caret =
            selections.items()[index].active.byte_offset.value();
        switch (kind) {
        case HistoryEditKind::typing:
            if (edit.erased_bytes != 0 || edit.inserted_text.empty() ||
                edit.offset.value() != caret) {
                return false;
            }
            break;
        case HistoryEditKind::delete_backward:
            if (!edit.inserted_text.empty() || edit.erased_bytes == 0 ||
                edit.offset.value() + edit.erased_bytes != caret) {
                return false;
            }
            break;
        case HistoryEditKind::delete_forward:
            if (!edit.inserted_text.empty() || edit.erased_bytes == 0 ||
                edit.offset.value() != caret) {
                return false;
            }
            break;
        case HistoryEditKind::other:
            return false;
        }
    }
    return true;
}

bool ranges_are_capturable(const DocumentSnapshot& before,
                           const EditTransaction& transaction) {
    for (const auto& edit : transaction.edits) {
        if (edit.offset.value() > before.text.size() ||
            edit.erased_bytes >
                before.text.size() -
                    static_cast<std::size_t>(edit.offset.value())) {
            return false;
        }
    }
    return true;
}

HistoryStep make_step(const DocumentSnapshot& before,
                      const EditTransaction& transaction) {
    HistoryStep step{{}, {}, 0};
    step.forward = transaction.edits;
    const auto ordered = ordered_edits(transaction.edits);
    std::int64_t displacement = 0;
    step.inverse.reserve(ordered.size());
    for (const auto* edit : ordered) {
        const auto offset = static_cast<std::int64_t>(edit->offset.value());
        const auto output_offset = offset + displacement;
        const auto erased = before.text.substr(
            static_cast<std::size_t>(edit->offset.value()),
            static_cast<std::size_t>(edit->erased_bytes));
        step.inverse.push_back(
            {ByteOffset{static_cast<std::uint64_t>(output_offset)},
             static_cast<std::uint64_t>(edit->inserted_text.size()), erased});
        step.payload_bytes = saturated_add(
            step.payload_bytes,
            saturated_add(static_cast<std::uint64_t>(edit->inserted_text.size()),
                          edit->erased_bytes));
        displacement +=
            static_cast<std::int64_t>(edit->inserted_text.size()) -
            static_cast<std::int64_t>(edit->erased_bytes);
    }
    return step;
}

std::uint64_t unit_charge(const HistoryStep& step,
                          const SelectionSet& before,
                          const SelectionSet& after) {
    return saturated_add(
        step.payload_bytes,
        saturated_add(selection_charge(before), selection_charge(after)));
}

}  // namespace

struct DocumentHistory::Impl {
    explicit Impl(HistoryConfig history_config) : config(history_config) {}

    HistoryConfig config;
    std::vector<HistoryUnit> undo;
    std::vector<HistoryUnit> redo;
    std::optional<Revision> expected_revision;
    std::uint64_t retained{0};
    bool barrier{true};

    void remove_charge(const HistoryUnit& unit) noexcept {
        retained -= unit.charged_bytes;
    }

    void clear_redo() noexcept {
        for (const auto& unit : redo) {
            remove_charge(unit);
        }
        redo.clear();
    }

    void clear_all() noexcept {
        undo.clear();
        redo.clear();
        retained = 0;
    }

    void enforce_budget() {
        while (retained > config.byte_budget && !undo.empty()) {
            remove_charge(undo.front());
            undo.erase(undo.begin());
        }
    }

    bool can_coalesce(const HistoryUnit& unit, const HistoryStep& step,
                      const SelectionSet& before, HistoryEditKind kind,
                      std::uint64_t timestamp_ms,
                      bool step_coalescible) const {
        (void)step;
        return !barrier && unit.coalescible && step_coalescible &&
               unit.kind == kind &&
               unit.selections_after == before &&
               timestamp_ms >= unit.last_timestamp_ms &&
               timestamp_ms - unit.last_timestamp_ms <= config.coalesce_ms;
    }
};

HistoryCommandSet::HistoryCommandSet()
    : descriptors_{{{"edit.undo", HistoryCommand::undo},
                    {"edit.redo", HistoryCommand::redo}}} {}

const std::array<HistoryCommandDescriptor, 2>&
HistoryCommandSet::descriptors() const noexcept {
    return descriptors_;
}

HistoryCommandSet history_command_set() {
    return HistoryCommandSet{};
}

HistoryDelta derive_history_delta(const HistoryViewState& before,
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

HistoryResult DocumentHistory::apply_edit(
    Document& document, const EditTransaction& transaction,
    const SelectionSet& selections_before,
    const SelectionSet& selections_after, HistoryEditKind kind,
    std::uint64_t timestamp_ms) {
    const auto before = document.snapshot();
    const bool bypassed =
        impl_->expected_revision &&
        *impl_->expected_revision != before.revision;
    if (!ranges_are_capturable(before, transaction)) {
        const auto result = document.apply(transaction);
        return failure(HistoryError::document_rejected, result.revision,
                       result.message, result.error);
    }

    auto step = make_step(before, transaction);
    const auto result = document.apply(transaction);
    if (!result.accepted()) {
        return failure(HistoryError::document_rejected, result.revision,
                       result.message, result.error);
    }

    if (bypassed) {
        impl_->clear_all();
        impl_->barrier = true;
    }
    impl_->clear_redo();

    const bool step_coalescible =
        edit_shape_matches(transaction, selections_before, kind);
    if (!impl_->undo.empty() &&
        impl_->can_coalesce(impl_->undo.back(), step, selections_before, kind,
                            timestamp_ms, step_coalescible)) {
        auto& unit = impl_->undo.back();
        impl_->retained -= unit.charged_bytes;
        unit.steps.push_back(std::move(step));
        unit.selections_after = selections_after;
        unit.last_timestamp_ms = timestamp_ms;
        unit.charged_bytes = saturated_add(
            unit.charged_bytes,
            unit.steps.back().payload_bytes);
        impl_->retained =
            saturated_add(impl_->retained, unit.charged_bytes);
    } else {
        const auto charge =
            unit_charge(step, selections_before, selections_after);
        impl_->undo.push_back(
            {{std::move(step)}, selections_before, selections_after, kind,
             timestamp_ms, charge, step_coalescible});
        impl_->retained = saturated_add(impl_->retained, charge);
    }

    impl_->enforce_budget();
    impl_->expected_revision = result.revision;
    impl_->barrier = false;
    return {HistoryError::none, DocumentError::none, result.revision,
            selections_after, {}};
}

HistoryResult DocumentHistory::undo(Document& document) {
    if (impl_->undo.empty()) {
        return failure(HistoryError::no_undo, document.revision(),
                       "document has no undo history");
    }
    if (!impl_->expected_revision ||
        document.revision() != *impl_->expected_revision) {
        return failure(HistoryError::stale_document, document.revision(),
                       "document revision changed outside its history");
    }

    auto& unit = impl_->undo.back();
    const auto revision = document.revision().value();
    if (unit.steps.size() >
        std::numeric_limits<std::uint64_t>::max() - revision) {
        return failure(HistoryError::revision_exhausted, document.revision(),
                       "undo would exhaust the document revision");
    }

    for (auto iterator = unit.steps.rbegin(); iterator != unit.steps.rend();
         ++iterator) {
        const auto result =
            document.apply({document.revision(), iterator->inverse});
        if (!result.accepted()) {
            return failure(HistoryError::document_rejected, result.revision,
                           result.message, result.error);
        }
    }

    auto moved = std::move(impl_->undo.back());
    impl_->undo.pop_back();
    const auto selections = moved.selections_before;
    impl_->redo.push_back(std::move(moved));
    impl_->expected_revision = document.revision();
    impl_->barrier = true;
    return {HistoryError::none, DocumentError::none, document.revision(),
            selections, {}};
}

HistoryResult DocumentHistory::redo(Document& document) {
    if (impl_->redo.empty()) {
        return failure(HistoryError::no_redo, document.revision(),
                       "document has no redo history");
    }
    if (!impl_->expected_revision ||
        document.revision() != *impl_->expected_revision) {
        return failure(HistoryError::stale_document, document.revision(),
                       "document revision changed outside its history");
    }

    auto& unit = impl_->redo.back();
    const auto revision = document.revision().value();
    if (unit.steps.size() >
        std::numeric_limits<std::uint64_t>::max() - revision) {
        return failure(HistoryError::revision_exhausted, document.revision(),
                       "redo would exhaust the document revision");
    }

    for (const auto& step : unit.steps) {
        const auto result =
            document.apply({document.revision(), step.forward});
        if (!result.accepted()) {
            return failure(HistoryError::document_rejected, result.revision,
                           result.message, result.error);
        }
    }

    auto moved = std::move(impl_->redo.back());
    impl_->redo.pop_back();
    const auto selections = moved.selections_after;
    impl_->undo.push_back(std::move(moved));
    impl_->expected_revision = document.revision();
    impl_->barrier = true;
    return {HistoryError::none, DocumentError::none, document.revision(),
            selections, {}};
}

void DocumentHistory::break_coalescing() noexcept {
    impl_->barrier = true;
}

bool DocumentHistory::can_undo() const noexcept {
    return !impl_->undo.empty();
}

bool DocumentHistory::can_redo() const noexcept {
    return !impl_->redo.empty();
}

std::uint64_t DocumentHistory::retained_bytes() const noexcept {
    return impl_->retained;
}

HistoryViewState DocumentHistory::view_state() const noexcept {
    return {can_undo(), can_redo(), retained_bytes()};
}

}  // namespace ssg
