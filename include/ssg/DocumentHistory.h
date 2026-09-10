#pragma once

#include <ssg/Settings.h>
#include <ssg/Document.h>
#include <ssg/Selection.h>
#include <ssg/TextInputCommands.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ssg {

enum class HistoryEditKind : std::uint8_t {
    Typing,
    DeleteBackward,
    DeleteForward,
    Other,
};

[[nodiscard]] inline HistoryEditKind historyEditKind(
    TextInputCommand command) noexcept {
    switch (command) {
        case TextInputCommand::Insert:
        case TextInputCommand::Newline:
            return HistoryEditKind::Typing;
        case TextInputCommand::DeleteBackward:
        case TextInputCommand::DeleteWordBackward:
            return HistoryEditKind::DeleteBackward;
        case TextInputCommand::DeleteForward:
        case TextInputCommand::DeleteWordForward:
            return HistoryEditKind::DeleteForward;
    }
    return HistoryEditKind::Other;
}

enum class HistoryError : std::uint8_t {
    None,
    NoUndo,
    NoRedo,
    StaleDocument,
    RevisionExhausted,
    DocumentRejected,
};

struct HistoryResult {
    HistoryError error;
    DocumentError documentError;
    std::uint64_t revision;
    std::optional<SelectionSet> selections;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == HistoryError::None;
    }
};

class DocumentHistory {
public:
    explicit DocumentHistory(const SettingsModel& settings);
    ~DocumentHistory();

    DocumentHistory(const DocumentHistory&) = delete;
    DocumentHistory& operator=(const DocumentHistory&) = delete;
    DocumentHistory(DocumentHistory&&) noexcept;
    DocumentHistory& operator=(DocumentHistory&&) noexcept;

    [[nodiscard]] HistoryResult applyEdit(
        Document& document, const EditTransaction& transaction,
        const SelectionSet& selectionsBefore,
        const SelectionSet& selectionsAfter, HistoryEditKind kind,
        std::uint64_t timestampMs);
    [[nodiscard]] HistoryResult undo(Document& document);
    [[nodiscard]] HistoryResult redo(Document& document);

    void breakCoalescing() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
