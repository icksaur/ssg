#pragma once

#include <ssg/Document.h>
#include <ssg/DocumentHistory.h>
#include <ssg/Selection.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

enum class ClipboardCommand : std::uint8_t {
    Copy,
    Cut,
    Paste,
};

// A copy or cut's text, offered to whatever client can reach a real system
// clipboard (the terminal writes it with OSC 52).
//
// FIRE AND FORGET, deliberately.  There is no response: the register already
// holds the text, so a local yank works whether or not the system clipboard
// accepted it, and reporting a failure the user cannot act on and does not
// otherwise notice would be worse than saying nothing.
//
// `id` increments per copy, so a client can write each one exactly once without
// the register ever learning that it did.
struct ClipboardWrite {
    std::uint64_t id;
    Revision requestRevision;
    std::string text;

    bool operator==(const ClipboardWrite&) const = default;
};

enum class ClipboardError : std::uint8_t {
    None,
    ReadOnly,
    Diff,
    InvalidSelection,
    InvalidUtf8,
    RequestExhausted,
    DocumentRejected,
};

struct ClipboardResult {
    ClipboardError error;
    Revision revision;
    std::optional<SelectionSet> selections;
    std::optional<ClipboardWrite> write;
    bool documentChanged;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ClipboardError::None;
    }
};

struct ClipboardViewState {
    std::vector<std::string> fragments;
    std::string plainText;
    // The most recent copy/cut, for a client that can push it to the system
    // clipboard.  Stays set: nothing reports back, so the register cannot know
    // when it has been served, and a client keys on the id instead.
    std::optional<ClipboardWrite> systemWrite;

    bool operator==(const ClipboardViewState&) const = default;
};

struct ClipboardDelta {
    bool changed;
    std::optional<ClipboardViewState> replacement;

    bool operator==(const ClipboardDelta&) const = default;
};

class ClipboardDeltaCodec {
public:
    [[nodiscard]] ClipboardDelta derive(const ClipboardViewState& before,
                                        const ClipboardViewState& after);
};

// CONTRACT
// ClipboardRegister: the register is authoritative for every editor operation;
//   the system clipboard is best-effort export only. There is no system
//   clipboard read and no acknowledgement of a write — both were tried and
//   retired — so no behavior may depend on either.
class ClipboardRegister {
public:
    explicit ClipboardRegister(int tabWidth = 4);
    ~ClipboardRegister();

    ClipboardRegister(const ClipboardRegister&) = delete;
    ClipboardRegister& operator=(const ClipboardRegister&) = delete;
    ClipboardRegister(ClipboardRegister&&) noexcept;
    ClipboardRegister& operator=(ClipboardRegister&&) noexcept;

    [[nodiscard]] ClipboardResult copy(
        const DocumentSnapshot& document, const SelectionSet& selections);
    [[nodiscard]] ClipboardResult cut(
        Document& document, DocumentHistory& history,
        const SelectionSet& selections, std::uint64_t timestampMs);
    // Pastes the register.  A read is meaningless without a response path (see
    // the ClipboardRegister contract).
    [[nodiscard]] ClipboardResult paste(
        Document& document, DocumentHistory& history,
        const SelectionSet& selections, std::uint64_t timestampMs);

    [[nodiscard]] ClipboardViewState viewState() const;

    // A monotonic counter that advances on every copy/cut (each mints a new
    // write id). A host reads it to detect that the register's content changed --
    // routing state for a paste-into-prompt -- without copying the text.
    [[nodiscard]] std::uint64_t writeGeneration() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
