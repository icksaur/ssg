#include <ssg/PromptEditState.h>

#include <ssg/GraphemeLayout.h>
#include <ssg/TextInputCommands.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

std::size_t previousGraphemeBoundary(std::string_view text,
                                     std::size_t cursor) {
    if (cursor == 0) return 0;
    const CellRun run = computeCellRun(text.substr(0, cursor));
    if (run.spans.empty()) return 0;
    return run.spans.back().byteOffset;
}

std::size_t nextGraphemeBoundary(std::string_view text, std::size_t cursor) {
    if (cursor >= text.size()) return text.size();
    const CellRun run = computeCellRun(text.substr(cursor));
    if (run.spans.empty()) return text.size();
    return cursor + run.spans.front().byteLen;
}

bool isGraphemeBoundary(std::string_view text, std::size_t cursor) {
    if (cursor == 0 || cursor == text.size()) return true;
    if (cursor > text.size()) return false;
    const CellRun run = computeCellRun(text);
    return std::any_of(run.spans.begin(), run.spans.end(),
                       [cursor](const CellSpan& span) {
                           return span.byteOffset == cursor;
                       });
}

std::size_t boundaryAtOrAfter(std::string_view text, std::size_t cursor) {
    if (cursor >= text.size()) return text.size();
    const CellRun run = computeCellRun(text);
    for (const auto& span : run.spans) {
        if (span.byteOffset >= cursor) return span.byteOffset;
        if (span.byteOffset + span.byteLen > cursor) {
            return span.byteOffset + span.byteLen;
        }
    }
    return text.size();
}

std::size_t wordBoundaryBackward(std::string_view text, std::size_t cursor) {
    std::size_t start = cursor;
    while (start > 0 &&
          !isWordByte(static_cast<unsigned char>(text[start - 1]))) {
        --start;
    }
    while (start > 0 &&
          isWordByte(static_cast<unsigned char>(text[start - 1]))) {
        --start;
    }
    return start;
}

std::size_t wordBoundaryForward(std::string_view text, std::size_t cursor) {
    const std::size_t n = text.size();
    std::size_t end = cursor;
    while (end < n && !isWordByte(static_cast<unsigned char>(text[end]))) {
        ++end;
    }
    while (end < n && isWordByte(static_cast<unsigned char>(text[end]))) {
        ++end;
    }
    return end;
}

}  // namespace

PromptEditState::PromptEditState(std::string text)
    : text_{std::move(text)}, cursor_{text_.size()} {}

PromptEditState::PromptEditState(std::string text, std::size_t cursor)
    : text_{std::move(text)}, cursor_{cursor} {
    if (!isGraphemeBoundary(text_, cursor_)) {
        throw std::invalid_argument{"prompt cursor is not a grapheme boundary"};
    }
}

PromptEditState applyPromptTextEdit(PromptEditState state,
                                    const PromptTextEdit& edit) {
    switch (edit.kind) {
    case PromptTextEdit::Kind::Insert:
        state.text_.insert(state.cursor_, edit.text);
        state.cursor_ += edit.text.size();
        state.cursor_ = boundaryAtOrAfter(state.text_, state.cursor_);
        return state;
    case PromptTextEdit::Kind::MoveLeft:
        state.cursor_ = previousGraphemeBoundary(state.text_, state.cursor_);
        return state;
    case PromptTextEdit::Kind::MoveRight:
        state.cursor_ = nextGraphemeBoundary(state.text_, state.cursor_);
        return state;
    case PromptTextEdit::Kind::MoveToStart:
        state.cursor_ = 0;
        return state;
    case PromptTextEdit::Kind::MoveToEnd:
        state.cursor_ = state.text_.size();
        return state;
    case PromptTextEdit::Kind::DeleteBackward: {
        const std::size_t start =
            previousGraphemeBoundary(state.text_, state.cursor_);
        state.text_.erase(start, state.cursor_ - start);
        state.cursor_ = start;
        state.cursor_ = boundaryAtOrAfter(state.text_, state.cursor_);
        return state;
    }
    case PromptTextEdit::Kind::DeleteForward: {
        const std::size_t end =
            nextGraphemeBoundary(state.text_, state.cursor_);
        state.text_.erase(state.cursor_, end - state.cursor_);
        state.cursor_ = boundaryAtOrAfter(state.text_, state.cursor_);
        return state;
    }
    case PromptTextEdit::Kind::DeleteWordBackward: {
        const std::size_t start =
            wordBoundaryBackward(state.text_, state.cursor_);
        state.text_.erase(start, state.cursor_ - start);
        state.cursor_ = start;
        state.cursor_ = boundaryAtOrAfter(state.text_, state.cursor_);
        return state;
    }
    case PromptTextEdit::Kind::DeleteWordForward: {
        const std::size_t end = wordBoundaryForward(state.text_, state.cursor_);
        state.text_.erase(state.cursor_, end - state.cursor_);
        state.cursor_ = boundaryAtOrAfter(state.text_, state.cursor_);
        return state;
    }
    }
    return state;
}

}  // namespace ssg
