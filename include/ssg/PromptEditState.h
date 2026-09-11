#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ssg {

struct PromptTextEdit;

class PromptEditState {
public:
    PromptEditState() = default;
    explicit PromptEditState(std::string text);
    // Throws when cursor is not a grapheme boundary in text.
    PromptEditState(std::string text, std::size_t cursor);

    friend bool operator==(const PromptEditState&,
                           const PromptEditState&) = default;

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }

private:
    std::string text_;
    std::size_t cursor_ = 0;

    friend PromptEditState applyPromptTextEdit(PromptEditState,
                                               const PromptTextEdit&);
};

struct PromptTextEdit {
    enum class Kind : std::uint8_t {
        Insert,
        MoveLeft,
        MoveRight,
        MoveToStart,
        MoveToEnd,
        DeleteBackward,
        DeleteForward,
        DeleteWordBackward,
        DeleteWordForward,
    } kind = Kind::Insert;
    std::string text;  // meaningful only for Kind::Insert

    friend bool operator==(const PromptTextEdit&,
                           const PromptTextEdit&) = default;
};

[[nodiscard]] PromptEditState applyPromptTextEdit(PromptEditState state,
                                                  const PromptTextEdit& edit);

}  // namespace ssg
