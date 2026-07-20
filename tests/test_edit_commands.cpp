#include <ssg/edit_commands.h>

#include "test_helpers.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using ssg::ByteOffset;
using ssg::Document;
using ssg::DocumentMode;
using ssg::DocumentPosition;
using ssg::EditCommand;
using ssg::EditCommandSettings;
using ssg::IndentStyle;
using ssg::LineEnding;
using ssg::Selection;
using ssg::SelectionSet;

DocumentPosition position(std::string_view text, std::uint64_t offset,
                          int tab_width = 4) {
    const auto value =
        ssg::resolveDocumentPosition(text, ByteOffset{offset}, tab_width);
    ASSERT_TRUE(value.has_value());
    return *value;
}

SelectionSet selections(
    std::string_view text,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> ranges,
    int tab_width = 4) {
    std::vector<Selection> values;
    for (const auto [anchor, active] : ranges) {
        values.push_back(
            {position(text, anchor, tab_width),
             position(text, active, tab_width)});
    }
    return SelectionSet{std::move(values)};
}

EditCommandSettings settings(
    IndentStyle style = IndentStyle::Spaces, std::uint32_t width = 2,
    LineEnding ending = LineEnding::Lf, std::string token = "//") {
    return {style, width, width, ending, std::move(token)};
}

std::string apply(Document& document, const ssg::EditCommandResult& result) {
    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.selections.has_value());
    if (result.transaction) {
        ASSERT_TRUE(document.apply(*result.transaction).accepted());
    }
    ASSERT_EQ(document.snapshot().text, result.resulting_text);
    return document.snapshot().text;
}

struct Fixture {
    const char* name;
    std::string input;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    EditCommand command;
    EditCommandSettings command_settings;
    std::string expected;
};

SelectionSet fixtureSelections(const Fixture& fixture) {
    std::vector<Selection> values;
    for (const auto [anchor, active] : fixture.ranges) {
        values.push_back({position(fixture.input, anchor),
                          position(fixture.input, active)});
    }
    return SelectionSet{std::move(values)};
}

void runFixture(const Fixture& fixture) {
    Document document{fixture.input};
    const auto result = ssg::applyEditCommand(
        document.snapshot(), fixtureSelections(fixture),
        fixture.command_settings, fixture.command);
    ASSERT_EQ(apply(document, result), fixture.expected);
}

TEST(commandSetIsExactAndImmutable) {
    static_assert(!std::is_copy_assignable_v<ssg::EditCommandSuiteCommandSet>);
    constexpr std::array<std::string_view, 13> expected{{
        "edit.indent",         "edit.outdent",
        "edit.duplicate_line", "edit.move_line_up",
        "edit.move_line_down", "edit.delete_line",
        "edit.join_lines",     "edit.uppercase",
        "edit.lowercase",      "edit.swap_case",
        "edit.sort_lines",     "edit.transpose",
        "edit.toggle_comment",
    }};
    const auto commands = ssg::editCommandSuiteCommandSet();
    ASSERT_EQ(commands.descriptors().size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT_EQ(commands.descriptors()[index].id, expected[index]);
    }
}

TEST(singleSelectionHandFixturesCoverEveryTransform) {
    const std::string combining = "e\xCC\x81x";
    const std::vector<Fixture> fixtures{
        {"indent", "a\nb", {{0, 0}}, EditCommand::Indent, settings(),
         "  a\nb"},
        {"outdent", "   a", {{3, 3}}, EditCommand::Outdent, settings(),
         " a"},
        {"duplicate-final-crlf", "a", {{0, 0}}, EditCommand::DuplicateLine,
         settings(IndentStyle::Spaces, 2, LineEnding::Crlf), "a\r\na"},
        {"move-up-crlf", "a\r\nb", {{3, 3}}, EditCommand::MoveLineUp,
         settings(), "b\r\na"},
        {"move-down-crlf", "a\r\nb", {{0, 0}}, EditCommand::MoveLineDown,
         settings(), "b\r\na"},
        {"delete-final-cr", "a\rb", {{2, 2}}, EditCommand::DeleteLine,
         settings(), "a"},
        {"join-crlf", "a\r\nb", {{0, 0}}, EditCommand::JoinLines,
         settings(), "a b"},
        {"uppercase", "aBc", {{0, 3}}, EditCommand::Uppercase, settings(),
         "ABC"},
        {"lowercase", "AbC", {{0, 3}}, EditCommand::Lowercase, settings(),
         "abc"},
        {"swap-case", "AbC", {{0, 3}}, EditCommand::SwapCase, settings(),
         "aBc"},
        {"sort-crlf", "b\r\na", {{0, 4}}, EditCommand::SortLines,
         settings(), "a\r\nb"},
        {"transpose-grapheme", combining, {{3, 3}}, EditCommand::Transpose,
         settings(), "xe\xCC\x81"},
        {"comment-after-indent", "  a\n  b", {{0, 7}},
         EditCommand::ToggleComment, settings(), "  //a\n  //b"},
    };
    for (const auto& fixture : fixtures) {
        runFixture(fixture);
    }
}

TEST(multipleSelectionHandFixturesCoverEveryTransform) {
    const std::vector<Fixture> fixtures{
        {"indent-tabs-cr", "a\rb\rc", {{0, 0}, {4, 4}},
         EditCommand::Indent,
         settings(IndentStyle::Tabs, 4, LineEnding::Cr), "\ta\rb\r\tc"},
        {"outdent-spaces-cr", "  a\r  b", {{2, 2}, {6, 6}},
         EditCommand::Outdent, settings(), "a\rb"},
        {"duplicate-disjoint", "a\nb\nc", {{0, 0}, {4, 4}},
         EditCommand::DuplicateLine, settings(), "a\na\nb\nc\nc"},
        {"move-up-disjoint", "a\nb\nc\nd", {{2, 2}, {6, 6}},
         EditCommand::MoveLineUp, settings(), "b\na\nd\nc"},
        {"move-down-disjoint", "a\nb\nc\nd", {{0, 0}, {4, 4}},
         EditCommand::MoveLineDown, settings(), "b\na\nd\nc"},
        {"delete-disjoint", "a\nb\nc\nd", {{0, 0}, {4, 4}},
         EditCommand::DeleteLine, settings(), "b\nd"},
        {"join-disjoint", "a\nb\nc\nd", {{0, 0}, {4, 4}},
         EditCommand::JoinLines, settings(), "a b\nc d"},
        {"uppercase-disjoint", "ab cd", {{0, 2}, {3, 5}},
         EditCommand::Uppercase, settings(), "AB CD"},
        {"lowercase-disjoint", "AB CD", {{0, 2}, {3, 5}},
         EditCommand::Lowercase, settings(), "ab cd"},
        {"swap-disjoint", "Ab cD", {{0, 2}, {3, 5}},
         EditCommand::SwapCase, settings(), "aB Cd"},
        {"sort-disjoint", "b\na\nx\nd\nc", {{0, 3}, {6, 9}},
         EditCommand::SortLines, settings(), "a\nb\nx\nc\nd"},
        {"transpose-disjoint", "ab cd", {{1, 1}, {4, 4}},
         EditCommand::Transpose, settings(), "ba dc"},
        {"comment-disjoint", "  a\nb\n  c", {{0, 0}, {6, 6}},
         EditCommand::ToggleComment, settings(), "  //a\nb\n  //c"},
    };
    for (const auto& fixture : fixtures) {
        runFixture(fixture);
    }
}

TEST(commentToggleRemovesOnlyWhenAllNonblankLinesAreCommented) {
    Document document{"  //a\n  \n\t//b"};
    auto result = ssg::applyEditCommand(
        document.snapshot(),
        selections(document.snapshot().text, {{0, 13}}, 2),
        settings(), EditCommand::ToggleComment);
    ASSERT_EQ(apply(document, result), std::string{"  a\n  \n\tb"});

    Document mixed{"//a\nb"};
    result = ssg::applyEditCommand(
        mixed.snapshot(), selections(mixed.snapshot().text, {{0, 5}}),
        settings(), EditCommand::ToggleComment);
    ASSERT_EQ(apply(mixed, result), std::string{"////a\n//b"});
}

TEST(selectionEndAtLineStartDoesNotTouchNextLine) {
    Document document{"a\nb"};
    const auto result = ssg::applyEditCommand(
        document.snapshot(), selections("a\nb", {{0, 2}}), settings(),
        EditCommand::Indent);
    ASSERT_EQ(apply(document, result), std::string{"  a\nb"});
}

TEST(resultSelectionsAreResolvedAgainstResultingText) {
    Document document{"a\nb"};
    const auto result = ssg::applyEditCommand(
        document.snapshot(), selections("a\nb", {{0, 0}, {2, 2}}),
        settings(), EditCommand::Indent);
    ASSERT_EQ(apply(document, result), std::string{"  a\n  b"});
    ASSERT_EQ(result.selections->items()[0].active.byte_offset,
              ByteOffset{2});
    ASSERT_EQ(result.selections->items()[1].active.byte_offset,
              ByteOffset{6});
}

TEST(displayTabWidthIsIndependentOfIndentWidth) {
    Document document{"\tabc"};
    auto command_settings = settings(IndentStyle::Spaces, 2);
    command_settings.tab_width = 4;
    const auto result = ssg::applyEditCommand(
        document.snapshot(), selections("\tabc", {{1, 1}}, 4),
        command_settings, EditCommand::Indent);
    ASSERT_EQ(apply(document, result), std::string{"  \tabc"});
    ASSERT_EQ(result.selections->primary().active.cell,
              ssg::CellIndex{4});
}

TEST(transposeAtDocumentEndIgnoresTrailingTerminator) {
    for (const std::string text : {"ab\n", "ab\r\n", "ab\r"}) {
        Document document{text};
        const auto result = ssg::applyEditCommand(
            document.snapshot(),
            selections(text, {{text.size(), text.size()}}), settings(),
            EditCommand::Transpose);
        ASSERT_EQ(apply(document, result),
                  std::string{"ba"} + text.substr(2));
    }
}

TEST(nonEditModesAndInvalidInputsFailAtomically) {
    for (const auto mode : {DocumentMode::ReadOnly, DocumentMode::Diff}) {
        Document document{"abc", mode};
        const auto before = document.snapshot();
        const auto result = ssg::applyEditCommand(
            before, selections("abc", {{0, 0}}), settings(),
            EditCommand::Indent);
        ASSERT_FALSE(result.accepted());
        ASSERT_FALSE(result.transaction.has_value());
        ASSERT_EQ(document.snapshot(), before);
    }

    Document document{"abc"};
    auto bad_settings = settings();
    bad_settings.indent_width = 0;
    auto result = ssg::applyEditCommand(
        document.snapshot(), selections("abc", {{0, 0}}), bad_settings,
        EditCommand::Indent);
    ASSERT_EQ(result.error, ssg::EditCommandError::InvalidSettings);

    bad_settings = settings();
    bad_settings.line_comment_token = "\n";
    result = ssg::applyEditCommand(
        document.snapshot(), selections("abc", {{0, 0}}), bad_settings,
        EditCommand::ToggleComment);
    ASSERT_EQ(result.error, ssg::EditCommandError::InvalidSettings);

    const DocumentPosition inconsistent{
        ByteOffset{1}, ssg::LineIndex{8}, ssg::CellIndex{8}};
    result = ssg::applyEditCommand(
        document.snapshot(),
        SelectionSet{{Selection{inconsistent, inconsistent}}}, settings(),
        EditCommand::Indent);
    ASSERT_EQ(result.error, ssg::EditCommandError::InvalidSelection);
    ASSERT_EQ(document.snapshot().text, std::string{"abc"});
}

TEST(unchangedTransformsAreExplicitNoops) {
    struct Noop {
        std::string text;
        std::pair<std::uint64_t, std::uint64_t> range;
        EditCommand command;
    };
    const std::vector<Noop> cases{
        {"abc", {0, 0}, EditCommand::Outdent},
        {"abc", {0, 0}, EditCommand::MoveLineUp},
        {"abc", {0, 0}, EditCommand::MoveLineDown},
        {"abc", {0, 0}, EditCommand::JoinLines},
        {"abc", {0, 0}, EditCommand::Uppercase},
        {"abc", {0, 0}, EditCommand::Lowercase},
        {"abc", {0, 0}, EditCommand::SwapCase},
        {"a\nb", {0, 3}, EditCommand::SortLines},
        {"abc", {0, 0}, EditCommand::Transpose},
    };
    for (const auto& test : cases) {
        Document document{test.text};
        const auto result = ssg::applyEditCommand(
            document.snapshot(), selections(test.text, {test.range}),
            settings(), test.command);
        ASSERT_TRUE(result.accepted());
        ASSERT_FALSE(result.transaction.has_value());
        ASSERT_EQ(apply(document, result), test.text);
        ASSERT_EQ(document.revision(), ssg::Revision{1});
    }
}

}  // namespace

int main() {
    RUN(commandSetIsExactAndImmutable);
    RUN(singleSelectionHandFixturesCoverEveryTransform);
    RUN(multipleSelectionHandFixturesCoverEveryTransform);
    RUN(commentToggleRemovesOnlyWhenAllNonblankLinesAreCommented);
    RUN(selectionEndAtLineStartDoesNotTouchNextLine);
    RUN(resultSelectionsAreResolvedAgainstResultingText);
    RUN(displayTabWidthIsIndependentOfIndentWidth);
    RUN(transposeAtDocumentEndIgnoresTrailingTerminator);
    RUN(nonEditModesAndInvalidInputsFailAtomically);
    RUN(unchangedTransformsAreExplicitNoops);
    return failed == 0 ? 0 : 1;
}
