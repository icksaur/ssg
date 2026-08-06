#pragma once

#include <ssg/DocumentHistory.h>
#include <ssg/TextInputCommands.h>

namespace ssg {

// How a text-input command coalesces in the undo history: consecutive edits of
// the same kind merge into one undo step, so typing is one unit until a delete
// (or a delete direction change) seals it. A value->value classification of the
// input-command vocabulary, kept as a free conversion in its own header so the
// two enum headers stay independent of each other.
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

}  // namespace ssg
