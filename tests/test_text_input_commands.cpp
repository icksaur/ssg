#include <ssg/TextInputCommands.h>
#include <ssg/DocumentHistory.h>

#include "test_helpers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
        ssg::SelectionNavigator::resolvePosition(text, ByteOffset{offset}, tabWidth);
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
        offsets.push_back(selection.active.byteOffset.value());
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
    ASSERT_EQ(document.snapshot().text, result.resultingText);
    return document.snapshot().text;
}

TEST(singleCaretInsertAndSelectionReplacement) {
    Document insertDocument{"abcd"};
    auto inserted = ssg::TextInputInterpreter{}.apply(
        insertDocument.snapshot(), selections("abcd", {{2, 2}}), settings(),
        TextInputCommand::Insert, {.text = "XY"});
    ASSERT_EQ(applyResult(insertDocument, inserted), std::string{"abXYcd"});
    ASSERT_EQ(caretOffsets(*inserted.selections),
              (std::vector<std::uint64_t>{4}));

    Document replaceDocument{"abcdef"};
    auto replaced = ssg::TextInputInterpreter{}.apply(
        replaceDocument.snapshot(), selections("abcdef", {{5, 2}}),
        settings(), TextInputCommand::Insert, {.text = "Q"});
    ASSERT_EQ(applyResult(replaceDocument, replaced),
              std::string{"abQf"});
    ASSERT_EQ(caretOffsets(*replaced.selections),
              (std::vector<std::uint64_t>{3}));
}

TEST(multipleCaretsInsertOnceEach) {
    Document document{"abcd"};
    auto result = ssg::TextInputInterpreter{}.apply(
        document.snapshot(), selections("abcd", {{1, 1}, {3, 3}}),
        settings(), TextInputCommand::Insert, {.text = "_"});

    ASSERT_EQ(applyResult(document, result), std::string{"a_bc_d"});
    ASSERT_EQ(caretOffsets(*result.selections),
              (std::vector<std::uint64_t>{2, 5}));
}

TEST(newlineUsesConfiguredEolAndIndentation) {
    Document crlfDocument{"    alpha"};
    auto crlf = ssg::TextInputInterpreter{}.apply(
        crlfDocument.snapshot(), selections("    alpha", {{9, 9}}),
        settings(LineEnding::Crlf, IndentStyle::Spaces, 4, true),
        TextInputCommand::Newline);
    ASSERT_EQ(applyResult(crlfDocument, crlf),
              std::string{"    alpha\r\n    "});
    ASSERT_EQ(caretOffsets(*crlf.selections),
              (std::vector<std::uint64_t>{15}));

    Document tabsDocument{" \t value"};
    auto tabs = ssg::TextInputInterpreter{}.apply(
        tabsDocument.snapshot(), selections(" \t value", {{8, 8}}),
        settings(LineEnding::Lf, IndentStyle::Tabs, 4, true),
        TextInputCommand::Newline);
    ASSERT_EQ(applyResult(tabsDocument, tabs),
              std::string{" \t value\n\t "});
}

TEST(mixedEolMatchesCurrentLineThenFallsBackToLf) {
    Document matchedDocument{"one\r\ntwo"};
    auto matched = ssg::TextInputInterpreter{}.apply(
        matchedDocument.snapshot(), selections("one\r\ntwo", {{1, 1}}),
        settings(LineEnding::Mixed), TextInputCommand::Newline);
    ASSERT_EQ(applyResult(matchedDocument, matched),
              std::string{"o\r\nne\r\ntwo"});

    Document fallbackDocument{"tail"};
    auto fallback = ssg::TextInputInterpreter{}.apply(
        fallbackDocument.snapshot(), selections("tail", {{4, 4}}),
        settings(LineEnding::Mixed), TextInputCommand::Newline);
    ASSERT_EQ(applyResult(fallbackDocument, fallback),
              std::string{"tail\n"});
}

TEST(characterDeletionUsesGraphemeClustersAndCrlf) {
    const std::string combining = "A" "e\xCC\x81" "B";
    Document backwardDocument{combining};
    auto backward = ssg::TextInputInterpreter{}.apply(
        backwardDocument.snapshot(), selections(combining, {{4, 4}}),
        settings(), TextInputCommand::DeleteBackward);
    ASSERT_EQ(applyResult(backwardDocument, backward), std::string{"AB"});
    ASSERT_EQ(caretOffsets(*backward.selections),
              (std::vector<std::uint64_t>{1}));

    const std::string family =
        "X\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9"
        "\xE2\x80\x8D\xF0\x9F\x91\xA7Y";
    Document forwardDocument{family};
    auto forward = ssg::TextInputInterpreter{}.apply(
        forwardDocument.snapshot(), selections(family, {{1, 1}}), settings(),
        TextInputCommand::DeleteForward);
    ASSERT_EQ(applyResult(forwardDocument, forward), std::string{"XY"});

    Document crlfDocument{"a\r\nb"};
    auto crlf = ssg::TextInputInterpreter{}.apply(
        crlfDocument.snapshot(), selections("a\r\nb", {{3, 3}}), settings(),
        TextInputCommand::DeleteBackward);
    ASSERT_EQ(applyResult(crlfDocument, crlf), std::string{"ab"});
}

TEST(wordDeletionAndSelectedDeletion) {
    Document backwardDocument{"alpha  beta!"};
    auto backward = ssg::TextInputInterpreter{}.apply(
        backwardDocument.snapshot(),
        selections("alpha  beta!", {{11, 11}}), settings(),
        TextInputCommand::DeleteWordBackward);
    ASSERT_EQ(applyResult(backwardDocument, backward),
              std::string{"alpha  !"});

    Document forwardDocument{"alpha  beta!"};
    auto forward = ssg::TextInputInterpreter{}.apply(
        forwardDocument.snapshot(), selections("alpha  beta!", {{5, 5}}),
        settings(), TextInputCommand::DeleteWordForward);
    ASSERT_EQ(applyResult(forwardDocument, forward),
              std::string{"alphabeta!"});

    Document selectedDocument{"012345"};
    auto selected = ssg::TextInputInterpreter{}.apply(
        selectedDocument.snapshot(), selections("012345", {{1, 4}}),
        settings(), TextInputCommand::DeleteForward);
    ASSERT_EQ(applyResult(selectedDocument, selected),
              std::string{"045"});
}

TEST(overlapNormalizationAndCoincidentCaretsEmitValidEdits) {
    Document overlapDocument{"abcdefgh"};
    auto overlap = ssg::TextInputInterpreter{}.apply(
        overlapDocument.snapshot(),
        selections("abcdefgh", {{1, 5}, {3, 7}}), settings(),
        TextInputCommand::Insert, {.text = "X"});
    ASSERT_EQ(applyResult(overlapDocument, overlap), std::string{"aXh"});
    ASSERT_EQ(overlap.transaction->edits.size(), 1u);

    const auto coincident = selections("abc", {{1, 1}, {1, 1}});
    ASSERT_EQ(coincident.items().size(), 1u);
    Document coincidentDocument{"abc"};
    auto oneInsert = ssg::TextInputInterpreter{}.apply(
        coincidentDocument.snapshot(), coincident, settings(),
        TextInputCommand::Insert, {.text = "X"});
    ASSERT_EQ(applyResult(coincidentDocument, oneInsert),
              std::string{"aXbc"});
    ASSERT_EQ(oneInsert.transaction->edits.size(), 1u);

    Document boundaryDocument{"abcdefgh"};
    auto boundary = ssg::TextInputInterpreter{}.apply(
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
    auto backward = ssg::TextInputInterpreter{}.apply(
        backwardDocument.snapshot(), selections("abc", {{0, 0}}), settings(),
        TextInputCommand::DeleteBackward);
    ASSERT_TRUE(backward.accepted());
    ASSERT_FALSE(backward.transaction.has_value());
    ASSERT_EQ(applyResult(backwardDocument, backward), std::string{"abc"});
    ASSERT_EQ(backwardDocument.revision(), std::uint64_t{1});

    Document forwardDocument{"abc"};
    auto forward = ssg::TextInputInterpreter{}.apply(
        forwardDocument.snapshot(), selections("abc", {{3, 3}}), settings(),
        TextInputCommand::DeleteForward);
    ASSERT_TRUE(forward.accepted());
    ASSERT_FALSE(forward.transaction.has_value());
    ASSERT_EQ(applyResult(forwardDocument, forward), std::string{"abc"});

    Document mixedDocument{"abc"};
    auto mixed = ssg::TextInputInterpreter{}.apply(
        mixedDocument.snapshot(), selections("abc", {{0, 0}, {3, 3}}),
        settings(), TextInputCommand::DeleteBackward);
    ASSERT_EQ(applyResult(mixedDocument, mixed), std::string{"ab"});
    ASSERT_EQ(caretOffsets(*mixed.selections),
              (std::vector<std::uint64_t>{0, 2}));
}

TEST(invalidInputAndNonEditModesFailAtomically) {
    Document document{"abc"};
    auto invalidUtf8 = ssg::TextInputInterpreter{}.apply(
        document.snapshot(), selections("abc", {{1, 1}}), settings(),
        TextInputCommand::Insert, {.text = std::string{"\xFF", 1}});
    ASSERT_EQ(invalidUtf8.error, TextInputError::InvalidUtf8);
    ASSERT_FALSE(invalidUtf8.accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"abc"});

    auto badPosition = position("abc", 1);
    badPosition.line = ssg::LineIndex{7};
    auto invalidSelection = ssg::TextInputInterpreter{}.apply(
        document.snapshot(),
        SelectionSet{{Selection{badPosition, badPosition}}}, settings(),
        TextInputCommand::Insert, {.text = "x"});
    ASSERT_EQ(invalidSelection.error, TextInputError::InvalidSelection);
    ASSERT_EQ(document.snapshot().text, std::string{"abc"});

    Document readOnly{"abc", DocumentMode::ReadOnly};
    auto rejected = ssg::TextInputInterpreter{}.apply(
        readOnly.snapshot(), selections("abc", {{1, 1}}), settings(),
        TextInputCommand::Insert, {.text = "x"});
    ASSERT_EQ(rejected.error, TextInputError::ReadOnly);
    ASSERT_EQ(readOnly.snapshot().text, std::string{"abc"});
}

}  // namespace

TEST(historyEditKindClassifiesEveryTextInputCommand) {
    using ssg::historyEditKind;
    ASSERT_TRUE(historyEditKind(TextInputCommand::Insert) ==
                ssg::HistoryEditKind::Typing);
    ASSERT_TRUE(historyEditKind(TextInputCommand::Newline) ==
                ssg::HistoryEditKind::Typing);
    ASSERT_TRUE(historyEditKind(TextInputCommand::DeleteBackward) ==
                ssg::HistoryEditKind::DeleteBackward);
    ASSERT_TRUE(historyEditKind(TextInputCommand::DeleteWordBackward) ==
                ssg::HistoryEditKind::DeleteBackward);
    ASSERT_TRUE(historyEditKind(TextInputCommand::DeleteForward) ==
                ssg::HistoryEditKind::DeleteForward);
    ASSERT_TRUE(historyEditKind(TextInputCommand::DeleteWordForward) ==
                ssg::HistoryEditKind::DeleteForward);
}

SSG_TEST_SUITE(test_text_input_commands) {
    RUN(singleCaretInsertAndSelectionReplacement);
    RUN(multipleCaretsInsertOnceEach);
    RUN(newlineUsesConfiguredEolAndIndentation);
    RUN(mixedEolMatchesCurrentLineThenFallsBackToLf);
    RUN(characterDeletionUsesGraphemeClustersAndCrlf);
    RUN(wordDeletionAndSelectedDeletion);
    RUN(overlapNormalizationAndCoincidentCaretsEmitValidEdits);
    RUN(boundaryDeletionIsSuccessfulNoop);
    RUN(invalidInputAndNonEditModesFailAtomically);
    RUN(historyEditKindClassifiesEveryTextInputCommand);
    return failed == 0 ? 0 : 1;
}
