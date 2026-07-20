#pragma once

#include <ssg/config.h>
#include <ssg/document.h>
#include <ssg/selection.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ssg {

enum class HistoryCommand : std::uint8_t {
    Undo,
    Redo,
};

struct HistoryCommandDescriptor {
    std::string_view id;
    HistoryCommand command;

    bool operator==(const HistoryCommandDescriptor&) const noexcept = default;
};

class HistoryCommandSet {
public:
    HistoryCommandSet(const HistoryCommandSet&) = default;
    HistoryCommandSet& operator=(const HistoryCommandSet&) = delete;

    [[nodiscard]] const std::array<HistoryCommandDescriptor, 2>&
    descriptors() const noexcept;

private:
    friend HistoryCommandSet historyCommandSet();
    HistoryCommandSet();

    const std::array<HistoryCommandDescriptor, 2> descriptors_;
};

[[nodiscard]] HistoryCommandSet historyCommandSet();

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
    Revision revision;
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

struct HistoryDelta {
    bool changed;
    std::optional<HistoryViewState> replacement;

    bool operator==(const HistoryDelta&) const noexcept = default;
};

class HistoryDeltaCodec {
public:
    [[nodiscard]] HistoryDelta derive(const HistoryViewState& before,
                                      const HistoryViewState& after);
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
