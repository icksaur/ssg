#include "test_helpers.h"

#include <ssg/ClipboardRegister.h>
#include <ssg/EditHistoryCoordinator.h>
#include <ssg/FindReplace.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

ssg::DocumentPosition position(std::string_view text, std::uint64_t offset) {
    return *ssg::SelectionNavigator::resolvePosition(text, ssg::ByteOffset{offset}, 4);
}

ssg::SelectionSet selections(
    std::string_view text,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> ranges) {
    std::vector<ssg::Selection> result;
    for (const auto [anchor, active] : ranges) {
        result.push_back(
            {position(text, anchor), position(text, active)});
    }
    return ssg::SelectionSet{std::move(result)};
}

ssg::TextInputSettings textSettings() {
    return {ssg::IndentStyle::Spaces, 4, false, ssg::LineEnding::Lf};
}

ssg::EditCommandSettings editSettings() {
    return {ssg::IndentStyle::Spaces, 2, 2, ssg::LineEnding::Lf, "//"};
}

TEST(everyMutatingCommandHasTheSpecifiedHistoryKind) {
    ASSERT_EQ(ssg::EditHistoryCoordinator::editKind(ssg::TextInputCommand::Insert),
              ssg::HistoryEditKind::Typing);
    ASSERT_EQ(ssg::EditHistoryCoordinator::editKind(
                  ssg::TextInputCommand::DeleteBackward),
              ssg::HistoryEditKind::DeleteBackward);
    ASSERT_EQ(ssg::EditHistoryCoordinator::editKind(
                  ssg::TextInputCommand::DeleteWordBackward),
              ssg::HistoryEditKind::DeleteBackward);
    ASSERT_EQ(ssg::EditHistoryCoordinator::editKind(
                  ssg::TextInputCommand::DeleteForward),
              ssg::HistoryEditKind::DeleteForward);
    ASSERT_EQ(ssg::EditHistoryCoordinator::editKind(
                  ssg::TextInputCommand::DeleteWordForward),
              ssg::HistoryEditKind::DeleteForward);
    ASSERT_EQ(ssg::EditHistoryCoordinator::editKind(ssg::TextInputCommand::Newline),
              ssg::HistoryEditKind::Other);

    constexpr std::array editCommands{
        ssg::EditCommand::Indent,
        ssg::EditCommand::Outdent,
        ssg::EditCommand::DuplicateLine,
        ssg::EditCommand::MoveLineUp,
        ssg::EditCommand::MoveLineDown,
        ssg::EditCommand::DeleteLine,
        ssg::EditCommand::JoinLines,
        ssg::EditCommand::Uppercase,
        ssg::EditCommand::Lowercase,
        ssg::EditCommand::SwapCase,
        ssg::EditCommand::SortLines,
        ssg::EditCommand::Transpose,
        ssg::EditCommand::ToggleComment,
    };
    for (const auto command : editCommands) {
        ASSERT_EQ(ssg::EditHistoryCoordinator::editKind(command),
                  ssg::HistoryEditKind::Other);
    }
}

TEST(noopAndRejectedCommandsDoNotCreateHistory) {
    ssg::Document document{"x"};
    ssg::DocumentHistory history;
    const auto atStart = selections("x", {{0, 0}});
    const auto noop = ssg::EditHistoryCoordinator{document, history}.applyTextInput(atStart, textSettings(),
        ssg::TextInputCommand::DeleteBackward, {}, 100);
    ASSERT_TRUE(noop.accepted());
    ASSERT_FALSE(noop.documentChanged());
    ASSERT_EQ(noop.selections, std::optional{atStart});
    ASSERT_FALSE(history.canUndo());

    ssg::Document readOnly{"x", ssg::DocumentMode::ReadOnly};
    const auto rejected = ssg::EditHistoryCoordinator{readOnly, history}.applyTextInput(atStart, textSettings(),
        ssg::TextInputCommand::Insert, {"y"}, 200);
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error,
              ssg::EditHistoryError::TextInputRejected);
    ASSERT_EQ(rejected.textInputError,
              std::optional{ssg::TextInputError::ReadOnly});
    ASSERT_EQ(readOnly.snapshot().text, std::string{"x"});
    ASSERT_FALSE(history.canUndo());
}

TEST(textInputSequenceHasHandAuthoredUndoBoundaries) {
    ssg::Document document{"word"};
    ssg::DocumentHistory history;
    auto current = selections("word", {{4, 4}});

    auto result = ssg::EditHistoryCoordinator{document, history}.applyTextInput(current, textSettings(),
        ssg::TextInputCommand::Insert, {"a"}, 100);
    ASSERT_TRUE(result.accepted());
    current = *result.selections;
    result = ssg::EditHistoryCoordinator{document, history}.applyTextInput(current, textSettings(),
        ssg::TextInputCommand::Insert, {"b"}, 200);
    ASSERT_TRUE(result.accepted());
    current = *result.selections;
    ASSERT_EQ(document.snapshot().text, std::string{"wordab"});

    result = ssg::EditHistoryCoordinator{document, history}.applyTextInput(current, textSettings(),
        ssg::TextInputCommand::Newline, {}, 300);
    ASSERT_TRUE(result.accepted());
    current = *result.selections;
    ASSERT_EQ(document.snapshot().text, std::string{"wordab\n"});

    auto undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"wordab"});
    ASSERT_EQ(undone.selections, std::optional{selections("wordab", {{6, 6}})});
    undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"word"});
    ASSERT_EQ(undone.selections, std::optional{selections("word", {{4, 4}})});
}

TEST(wordAndCharacterDeleteCoalesceByDirection) {
    ssg::Document document{"one two!"};
    ssg::DocumentHistory history;
    auto current = selections("one two!", {{8, 8}});

    auto result = ssg::EditHistoryCoordinator{document, history}.applyTextInput(current, textSettings(),
        ssg::TextInputCommand::DeleteBackward, {}, 100);
    ASSERT_TRUE(result.accepted());
    current = *result.selections;
    result = ssg::EditHistoryCoordinator{document, history}.applyTextInput(current, textSettings(),
        ssg::TextInputCommand::DeleteWordBackward, {}, 200);
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"one "});

    const auto undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"one two!"});
    ASSERT_FALSE(history.canUndo());
}

TEST(lineTransformIsAHistoryBarrierAndRestoresSelection) {
    ssg::Document document{"a\nb\n"};
    ssg::DocumentHistory history;
    auto current = selections("a\nb\n", {{0, 0}});

    auto typed = ssg::EditHistoryCoordinator{document, history}.applyTextInput(current, textSettings(),
        ssg::TextInputCommand::Insert, {"x"}, 100);
    ASSERT_TRUE(typed.accepted());
    current = *typed.selections;
    const auto transformed = ssg::EditHistoryCoordinator{document, history}.applyEditCommand(current, editSettings(),
        ssg::EditCommand::DuplicateLine, 200);
    ASSERT_TRUE(transformed.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"xa\nxa\nb\n"});

    auto undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"xa\nb\n"});
    ASSERT_EQ(undone.selections, typed.selections);
    undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"a\nb\n"});
    ASSERT_EQ(undone.selections, std::optional{selections("a\nb\n", {{0, 0}})});
}

TEST(clipboardAndReplaceAreDistinctNoncoalescingUnits) {
    ssg::Document document{"cat cat"};
    ssg::DocumentHistory history;
    ssg::ClipboardRegister clipboard;
    auto current = selections("cat cat", {{0, 3}});

    const auto cut = clipboard.cut(document, history, current, 100);
    ASSERT_TRUE(cut.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{" cat"});
    current = *cut.selections;

    const auto paste = clipboard.paste(
        document, history, current, 200);
    ASSERT_TRUE(paste.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"cat cat"});

    ssg::FindReplaceController find;
    find.openReplace(document.snapshot(), {"cat", {}, std::nullopt});
    const auto beforeReplace = *paste.selections;
    const auto afterReplace = selections("dog cat", {{3, 3}});
    const auto replaced = find.replaceCurrent(
        document, history, beforeReplace, afterReplace, "dog", 300);
    ASSERT_TRUE(replaced.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"dog cat"});

    auto undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"cat cat"});
    ASSERT_EQ(undone.selections, std::optional{beforeReplace});
    undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{" cat"});
    undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"cat cat"});
    ASSERT_EQ(undone.selections, std::optional{selections("cat cat", {{0, 3}})});
}

}  // namespace

int main() {
    RUN(everyMutatingCommandHasTheSpecifiedHistoryKind);
    RUN(noopAndRejectedCommandsDoNotCreateHistory);
    RUN(textInputSequenceHasHandAuthoredUndoBoundaries);
    RUN(wordAndCharacterDeleteCoalesceByDirection);
    RUN(lineTransformIsAHistoryBarrierAndRestoresSelection);
    RUN(clipboardAndReplaceAreDistinctNoncoalescingUnits);
    return failed == 0 ? 0 : 1;
}
