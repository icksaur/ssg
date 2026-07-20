#include "test_helpers.h"

#include <ssg/clipboard.h>
#include <ssg/edit_history_integration.h>
#include <ssg/find_replace.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

ssg::DocumentPosition position(std::string_view text, std::uint64_t offset) {
    return *ssg::resolve_document_position(text, ssg::ByteOffset{offset}, 4);
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

ssg::TextInputSettings text_settings() {
    return {ssg::IndentStyle::Spaces, 4, false, ssg::LineEnding::Lf};
}

ssg::EditCommandSettings edit_settings() {
    return {ssg::IndentStyle::Spaces, 2, 2, ssg::LineEnding::Lf, "//"};
}

TEST(every_mutating_command_has_the_specified_history_kind) {
    ASSERT_EQ(ssg::history_edit_kind(ssg::TextInputCommand::Insert),
              ssg::HistoryEditKind::Typing);
    ASSERT_EQ(ssg::history_edit_kind(
                  ssg::TextInputCommand::DeleteBackward),
              ssg::HistoryEditKind::DeleteBackward);
    ASSERT_EQ(ssg::history_edit_kind(
                  ssg::TextInputCommand::DeleteWordBackward),
              ssg::HistoryEditKind::DeleteBackward);
    ASSERT_EQ(ssg::history_edit_kind(
                  ssg::TextInputCommand::DeleteForward),
              ssg::HistoryEditKind::DeleteForward);
    ASSERT_EQ(ssg::history_edit_kind(
                  ssg::TextInputCommand::DeleteWordForward),
              ssg::HistoryEditKind::DeleteForward);
    ASSERT_EQ(ssg::history_edit_kind(ssg::TextInputCommand::Newline),
              ssg::HistoryEditKind::Other);

    constexpr std::array edit_commands{
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
    for (const auto command : edit_commands) {
        ASSERT_EQ(ssg::history_edit_kind(command),
                  ssg::HistoryEditKind::Other);
    }
}

TEST(noop_and_rejected_commands_do_not_create_history) {
    ssg::Document document{"x"};
    ssg::DocumentHistory history;
    const auto at_start = selections("x", {{0, 0}});
    const auto noop = ssg::apply_text_input_with_history(
        document, history, at_start, text_settings(),
        ssg::TextInputCommand::DeleteBackward, {}, 100);
    ASSERT_TRUE(noop.accepted());
    ASSERT_FALSE(noop.document_changed());
    ASSERT_EQ(noop.selections, std::optional{at_start});
    ASSERT_FALSE(history.can_undo());

    ssg::Document read_only{"x", ssg::DocumentMode::ReadOnly};
    const auto rejected = ssg::apply_text_input_with_history(
        read_only, history, at_start, text_settings(),
        ssg::TextInputCommand::Insert, {"y"}, 200);
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error,
              ssg::EditHistoryIntegrationError::TextInputRejected);
    ASSERT_EQ(rejected.text_input_error,
              std::optional{ssg::TextInputError::ReadOnly});
    ASSERT_EQ(read_only.snapshot().text, std::string{"x"});
    ASSERT_FALSE(history.can_undo());
}

TEST(text_input_sequence_has_hand_authored_undo_boundaries) {
    ssg::Document document{"word"};
    ssg::DocumentHistory history;
    auto current = selections("word", {{4, 4}});

    auto result = ssg::apply_text_input_with_history(
        document, history, current, text_settings(),
        ssg::TextInputCommand::Insert, {"a"}, 100);
    ASSERT_TRUE(result.accepted());
    current = *result.selections;
    result = ssg::apply_text_input_with_history(
        document, history, current, text_settings(),
        ssg::TextInputCommand::Insert, {"b"}, 200);
    ASSERT_TRUE(result.accepted());
    current = *result.selections;
    ASSERT_EQ(document.snapshot().text, std::string{"wordab"});

    result = ssg::apply_text_input_with_history(
        document, history, current, text_settings(),
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

TEST(word_and_character_delete_coalesce_by_direction) {
    ssg::Document document{"one two!"};
    ssg::DocumentHistory history;
    auto current = selections("one two!", {{8, 8}});

    auto result = ssg::apply_text_input_with_history(
        document, history, current, text_settings(),
        ssg::TextInputCommand::DeleteBackward, {}, 100);
    ASSERT_TRUE(result.accepted());
    current = *result.selections;
    result = ssg::apply_text_input_with_history(
        document, history, current, text_settings(),
        ssg::TextInputCommand::DeleteWordBackward, {}, 200);
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"one "});

    const auto undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"one two!"});
    ASSERT_FALSE(history.can_undo());
}

TEST(line_transform_is_a_history_barrier_and_restores_selection) {
    ssg::Document document{"a\nb\n"};
    ssg::DocumentHistory history;
    auto current = selections("a\nb\n", {{0, 0}});

    auto typed = ssg::apply_text_input_with_history(
        document, history, current, text_settings(),
        ssg::TextInputCommand::Insert, {"x"}, 100);
    ASSERT_TRUE(typed.accepted());
    current = *typed.selections;
    const auto transformed = ssg::apply_edit_command_with_history(
        document, history, current, edit_settings(),
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

TEST(clipboard_and_replace_are_distinct_noncoalescing_units) {
    ssg::Document document{"cat cat"};
    ssg::DocumentHistory history;
    ssg::ClipboardRegister clipboard;
    auto current = selections("cat cat", {{0, 3}});

    const auto cut = clipboard.cut(document, history, current, 100);
    ASSERT_TRUE(cut.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{" cat"});
    current = *cut.selections;

    const auto paste = clipboard.paste(
        document, history, current, ssg::ClipboardPasteMode::InternalOnly,
        200);
    ASSERT_TRUE(paste.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"cat cat"});

    ssg::FindReplaceController find;
    find.open_replace(document.snapshot(), {"cat", {}, std::nullopt});
    const auto before_replace = *paste.selections;
    const auto after_replace = selections("dog cat", {{3, 3}});
    const auto replaced = find.replace_current(
        document, history, before_replace, after_replace, "dog", 300);
    ASSERT_TRUE(replaced.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"dog cat"});

    auto undone = history.undo(document);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"cat cat"});
    ASSERT_EQ(undone.selections, std::optional{before_replace});
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
    RUN(every_mutating_command_has_the_specified_history_kind);
    RUN(noop_and_rejected_commands_do_not_create_history);
    RUN(text_input_sequence_has_hand_authored_undo_boundaries);
    RUN(word_and_character_delete_coalesce_by_direction);
    RUN(line_transform_is_a_history_barrier_and_restores_selection);
    RUN(clipboard_and_replace_are_distinct_noncoalescing_units);
    return failed == 0 ? 0 : 1;
}
