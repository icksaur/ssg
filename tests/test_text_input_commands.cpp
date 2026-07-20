#include <ssg/text_input_commands.h>

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
using ssg::IndentStyle;
using ssg::LineEnding;
using ssg::Selection;
using ssg::SelectionSet;
using ssg::TextInputCommand;
using ssg::TextInputError;
using ssg::TextInputSettings;

DocumentPosition position(std::string_view text, std::uint64_t offset,
                          int tabWidth = 4) {
    const auto resolved =
        ssg::resolveDocumentPosition(text, ByteOffset{offset}, tabWidth);
    ASSERT_TRUE(resolved.has_value());
    return resolved.value();
}

SelectionSet selections(
    std::string_view text,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> ranges,
    int tabWidth = 4) {
    std::vector<Selection> values;
    for (const auto [anchor, active] : ranges) {
        values.push_back(
            Selection{position(text, anchor, tabWidth),
                      position(text, active, tabWidth)});
    }
    return SelectionSet{std::move(values)};
}

TextInputSettings settings(
    LineEnding ending = LineEnding::Lf,
    IndentStyle style = IndentStyle::Spaces, std::uint32_t width = 4,
    bool autoIndent = false) {
    return TextInputSettings{style, width, autoIndent, ending};
}

std::vector<std::uint64_t> caretOffsets(const SelectionSet& value) {
    std::vector<std::uint64_t> offsets;
    for (const auto& selection : value.items()) {
        ASSERT_TRUE(selection.isCaret());
        offsets.push_back(selection.active.byte_offset.value());
    }
    return offsets;
}

std::string applyResult(Document& document,
                         const ssg::TextInputResult& result) {
    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.selections.has_value());
    if (result.transaction.has_value()) {
        const auto applied = document.apply(*result.transaction);
        ASSERT_TRUE(applied.accepted());
    }
    ASSERT_EQ(document.snapshot().text, result.resulting_text);
    return document.snapshot().text;
}

TEST(commandSetIsExactAndImmutable) {
    static_assert(!std::is_copy_assignable_v<ssg::TextInputCommandSet>);
    constexpr std::array<std::string_view, 6> expected{{
        "text.insert",
        "text.newline",
        "text.delete_backward",
        "text.delete_forward",
        "text.delete_word_backward",
        "text.delete_word_forward",
    }};

    const auto commands = ssg::textInputCommandSet();
    ASSERT_EQ(commands.descriptors().size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT_EQ(commands.descriptors()[index].id, expected[index]);
    }
}

TEST(singleCaretInsertAndSelectionReplacement) {
    Document insertDocument{"abcd"};
    auto inserted = ssg::applyTextInput(
        insertDocument.snapshot(), selections("abcd", {{2, 2}}), settings(),
        TextInputCommand::Insert, {.text = "XY"});
    ASSERT_EQ(applyResult(insertDocument, inserted), std::string{"abXYcd"});
    ASSERT_EQ(caretOffsets(*inserted.selections),
              (std::vector<std::uint64_t>{4}));

    Document replaceDocument{"abcdef"};
    auto replaced = ssg::applyTextInput(
        replaceDocument.snapshot(), selections("abcdef", {{5, 2}}),
        settings(), TextInputCommand::Insert, {.text = "Q"});
    ASSERT_EQ(applyResult(replaceDocument, replaced),
              std::string{"abQf"});
    ASSERT_EQ(caretOffsets(*replaced.selections),
              (std::vector<std::uint64_t>{3}));
}

TEST(multipleCaretsInsertOnceEach) {
    Document document{"abcd"};
    auto result = ssg::applyTextInput(
        document.snapshot(), selections("abcd", {{1, 1}, {3, 3}}),
        settings(), TextInputCommand::Insert, {.text = "_"});

    ASSERT_EQ(applyResult(document, result), std::string{"a_bc_d"});
    ASSERT_EQ(caretOffsets(*result.selections),
              (std::vector<std::uint64_t>{2, 5}));
}

TEST(newlineUsesConfiguredEolAndIndentation) {
    Document crlfDocument{"    alpha"};
    auto crlf = ssg::applyTextInput(
        crlfDocument.snapshot(), selections("    alpha", {{9, 9}}),
        settings(LineEnding::Crlf, IndentStyle::Spaces, 4, true),
        TextInputCommand::Newline);
    ASSERT_EQ(applyResult(crlfDocument, crlf),
              std::string{"    alpha\r\n    "});
    ASSERT_EQ(caretOffsets(*crlf.selections),
              (std::vector<std::uint64_t>{15}));

    Document tabsDocument{" \t value"};
    auto tabs = ssg::applyTextInput(
        tabsDocument.snapshot(), selections(" \t value", {{8, 8}}),
        settings(LineEnding::Lf, IndentStyle::Tabs, 4, true),
        TextInputCommand::Newline);
    ASSERT_EQ(applyResult(tabsDocument, tabs),
              std::string{" \t value\n\t "});
}

TEST(mixedEolMatchesCurrentLineThenFallsBackToLf) {
    Document matchedDocument{"one\r\ntwo"};
    auto matched = ssg::applyTextInput(
        matchedDocument.snapshot(), selections("one\r\ntwo", {{1, 1}}),
        settings(LineEnding::Mixed), TextInputCommand::Newline);
    ASSERT_EQ(applyResult(matchedDocument, matched),
              std::string{"o\r\nne\r\ntwo"});

    Document fallbackDocument{"tail"};
    auto fallback = ssg::applyTextInput(
        fallbackDocument.snapshot(), selections("tail", {{4, 4}}),
        settings(LineEnding::Mixed), TextInputCommand::Newline);
    ASSERT_EQ(applyResult(fallbackDocument, fallback),
              std::string{"tail\n"});
}

TEST(characterDeletionUsesGraphemeClustersAndCrlf) {
    const std::string combining = "A" "e\xCC\x81" "B";
    Document backwardDocument{combining};
    auto backward = ssg::applyTextInput(
        backwardDocument.snapshot(), selections(combining, {{4, 4}}),
        settings(), TextInputCommand::DeleteBackward);
    ASSERT_EQ(applyResult(backwardDocument, backward), std::string{"AB"});
    ASSERT_EQ(caretOffsets(*backward.selections),
              (std::vector<std::uint64_t>{1}));

    const std::string family =
        "X\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9"
        "\xE2\x80\x8D\xF0\x9F\x91\xA7Y";
    Document forwardDocument{family};
    auto forward = ssg::applyTextInput(
        forwardDocument.snapshot(), selections(family, {{1, 1}}), settings(),
        TextInputCommand::DeleteForward);
    ASSERT_EQ(applyResult(forwardDocument, forward), std::string{"XY"});

    Document crlfDocument{"a\r\nb"};
    auto crlf = ssg::applyTextInput(
        crlfDocument.snapshot(), selections("a\r\nb", {{3, 3}}), settings(),
        TextInputCommand::DeleteBackward);
    ASSERT_EQ(applyResult(crlfDocument, crlf), std::string{"ab"});
}

TEST(wordDeletionAndSelectedDeletion) {
    Document backwardDocument{"alpha  beta!"};
    auto backward = ssg::applyTextInput(
        backwardDocument.snapshot(),
        selections("alpha  beta!", {{11, 11}}), settings(),
        TextInputCommand::DeleteWordBackward);
    ASSERT_EQ(applyResult(backwardDocument, backward),
              std::string{"alpha  !"});

    Document forwardDocument{"alpha  beta!"};
    auto forward = ssg::applyTextInput(
        forwardDocument.snapshot(), selections("alpha  beta!", {{5, 5}}),
        settings(), TextInputCommand::DeleteWordForward);
    ASSERT_EQ(applyResult(forwardDocument, forward),
              std::string{"alphabeta!"});

    Document selectedDocument{"012345"};
    auto selected = ssg::applyTextInput(
        selectedDocument.snapshot(), selections("012345", {{1, 4}}),
        settings(), TextInputCommand::DeleteForward);
    ASSERT_EQ(applyResult(selectedDocument, selected),
              std::string{"045"});
}

TEST(overlapNormalizationAndCoincidentCaretsEmitValidEdits) {
    Document overlapDocument{"abcdefgh"};
    auto overlap = ssg::applyTextInput(
        overlapDocument.snapshot(),
        selections("abcdefgh", {{1, 5}, {3, 7}}), settings(),
        TextInputCommand::Insert, {.text = "X"});
    ASSERT_EQ(applyResult(overlapDocument, overlap), std::string{"aXh"});
    ASSERT_EQ(overlap.transaction->edits.size(), 1u);

    const auto coincident = selections("abc", {{1, 1}, {1, 1}});
    ASSERT_EQ(coincident.items().size(), 1u);
    Document coincidentDocument{"abc"};
    auto oneInsert = ssg::applyTextInput(
        coincidentDocument.snapshot(), coincident, settings(),
        TextInputCommand::Insert, {.text = "X"});
    ASSERT_EQ(applyResult(coincidentDocument, oneInsert),
              std::string{"aXbc"});
    ASSERT_EQ(oneInsert.transaction->edits.size(), 1u);

    Document boundaryDocument{"abcdefgh"};
    auto boundary = ssg::applyTextInput(
        boundaryDocument.snapshot(),
        selections("abcdefgh", {{2, 2}, {2, 5}}), settings(),
        TextInputCommand::Insert, {.text = "Q"});
    ASSERT_EQ(applyResult(boundaryDocument, boundary),
              std::string{"abQfgh"});
    ASSERT_EQ(boundary.transaction->edits.size(), 1u);
    ASSERT_EQ(caretOffsets(*boundary.selections),
              (std::vector<std::uint64_t>{3}));
}

TEST(boundaryDeletionIsSuccessfulNoop) {
    Document backwardDocument{"abc"};
    auto backward = ssg::applyTextInput(
        backwardDocument.snapshot(), selections("abc", {{0, 0}}), settings(),
        TextInputCommand::DeleteBackward);
    ASSERT_TRUE(backward.accepted());
    ASSERT_FALSE(backward.transaction.has_value());
    ASSERT_EQ(applyResult(backwardDocument, backward), std::string{"abc"});
    ASSERT_EQ(backwardDocument.revision(), ssg::Revision{1});

    Document forwardDocument{"abc"};
    auto forward = ssg::applyTextInput(
        forwardDocument.snapshot(), selections("abc", {{3, 3}}), settings(),
        TextInputCommand::DeleteForward);
    ASSERT_TRUE(forward.accepted());
    ASSERT_FALSE(forward.transaction.has_value());
    ASSERT_EQ(applyResult(forwardDocument, forward), std::string{"abc"});

    Document mixedDocument{"abc"};
    auto mixed = ssg::applyTextInput(
        mixedDocument.snapshot(), selections("abc", {{0, 0}, {3, 3}}),
        settings(), TextInputCommand::DeleteBackward);
    ASSERT_EQ(applyResult(mixedDocument, mixed), std::string{"ab"});
    ASSERT_EQ(caretOffsets(*mixed.selections),
              (std::vector<std::uint64_t>{0, 2}));
}

TEST(invalidInputAndNonEditModesFailAtomically) {
    Document document{"abc"};
    auto invalidUtf8 = ssg::applyTextInput(
        document.snapshot(), selections("abc", {{1, 1}}), settings(),
        TextInputCommand::Insert, {.text = std::string{"\xFF", 1}});
    ASSERT_EQ(invalidUtf8.error, TextInputError::InvalidUtf8);
    ASSERT_FALSE(invalidUtf8.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"abc"});

    auto badPosition = position("abc", 1);
    badPosition.line = ssg::LineIndex{7};
    auto invalidSelection = ssg::applyTextInput(
        document.snapshot(),
        SelectionSet{{Selection{badPosition, badPosition}}}, settings(),
        TextInputCommand::Insert, {.text = "x"});
    ASSERT_EQ(invalidSelection.error, TextInputError::InvalidSelection);
    ASSERT_EQ(document.snapshot().text, std::string{"abc"});

    Document readOnly{"abc", DocumentMode::ReadOnly};
    auto rejected = ssg::applyTextInput(
        readOnly.snapshot(), selections("abc", {{1, 1}}), settings(),
        TextInputCommand::Insert, {.text = "x"});
    ASSERT_EQ(rejected.error, TextInputError::ReadOnly);
    ASSERT_EQ(readOnly.snapshot().text, std::string{"abc"});
}

}  // namespace

int main() {
    RUN(commandSetIsExactAndImmutable);
    RUN(singleCaretInsertAndSelectionReplacement);
    RUN(multipleCaretsInsertOnceEach);
    RUN(newlineUsesConfiguredEolAndIndentation);
    RUN(mixedEolMatchesCurrentLineThenFallsBackToLf);
    RUN(characterDeletionUsesGraphemeClustersAndCrlf);
    RUN(wordDeletionAndSelectedDeletion);
    RUN(overlapNormalizationAndCoincidentCaretsEmitValidEdits);
    RUN(boundaryDeletionIsSuccessfulNoop);
    RUN(invalidInputAndNonEditModesFailAtomically);
    return failed == 0 ? 0 : 1;
}
