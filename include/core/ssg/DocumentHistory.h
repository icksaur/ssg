#pragma once

#include <ssg/config.h>
#include <ssg/Document.h>
#include <ssg/Selection.h>

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

struct HistoryViewState {
    bool canUndo;
    bool canRedo;
    std::uint64_t retainedBytes;

    bool operator==(const HistoryViewState&) const noexcept = default;
};

class DocumentHistory {
public:
    explicit DocumentHistory(HistoryConfig config = HistoryConfig::defaults());
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

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] std::uint64_t retainedBytes() const noexcept;
    [[nodiscard]] HistoryViewState viewState() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
