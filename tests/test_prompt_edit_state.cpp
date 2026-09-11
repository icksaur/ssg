#include <ssg/PromptEditState.h>

#include "test_helpers.h"

#include <string>

namespace {

using namespace ssg;

PromptEditState apply(PromptEditState state, PromptTextEdit::Kind kind,
                      std::string text = {}) {
    return applyPromptTextEdit(std::move(state), {kind, std::move(text)});
}

TEST(insertLandsAtTheCursorNotAlwaysAtTheEnd) {
    const auto result =
        apply({"ac", 1}, PromptTextEdit::Kind::Insert, "b");
    ASSERT_EQ(result.text(), std::string{"abc"});
    ASSERT_EQ(result.cursor(), static_cast<std::size_t>(2));
}

TEST(moveLeftStepsBackOneGraphemeAndStopsAtStart) {
    PromptEditState state{"ab", 2};
    state = apply(state, PromptTextEdit::Kind::MoveLeft);
    ASSERT_EQ(state.cursor(), static_cast<std::size_t>(1));
    state = apply(state, PromptTextEdit::Kind::MoveLeft);
    ASSERT_EQ(state.cursor(), static_cast<std::size_t>(0));
    // Already at the start: MoveLeft is a no-op, not a fault.
    state = apply(state, PromptTextEdit::Kind::MoveLeft);
    ASSERT_EQ(state.cursor(), static_cast<std::size_t>(0));
}

TEST(moveRightStepsForwardOneGraphemeAndStopsAtEnd) {
    PromptEditState state{"ab", 0};
    state = apply(state, PromptTextEdit::Kind::MoveRight);
    ASSERT_EQ(state.cursor(), static_cast<std::size_t>(1));
    state = apply(state, PromptTextEdit::Kind::MoveRight);
    ASSERT_EQ(state.cursor(), static_cast<std::size_t>(2));
    // Already at the end: MoveRight is a no-op, not a fault.
    state = apply(state, PromptTextEdit::Kind::MoveRight);
    ASSERT_EQ(state.cursor(), static_cast<std::size_t>(2));
}

TEST(moveToStartAndMoveToEndJumpDirectlyToTheEdges) {
    PromptEditState state{"hello", 2};
    state = apply(state, PromptTextEdit::Kind::MoveToStart);
    ASSERT_EQ(state.cursor(), static_cast<std::size_t>(0));
    state = apply(state, PromptTextEdit::Kind::MoveToEnd);
    ASSERT_EQ(state.cursor(), static_cast<std::size_t>(5));
}

TEST(deleteForwardRemovesTheGraphemeAfterTheCursorAndKeepsCursorPut) {
    const auto result =
        apply({"abc", 1}, PromptTextEdit::Kind::DeleteForward);
    ASSERT_EQ(result.text(), std::string{"ac"});
    ASSERT_EQ(result.cursor(), static_cast<std::size_t>(1));
}

TEST(deleteForwardAtEndOfTextIsANoOp) {
    const auto result =
        apply({"abc", 3}, PromptTextEdit::Kind::DeleteForward);
    ASSERT_EQ(result.text(), std::string{"abc"});
    ASSERT_EQ(result.cursor(), static_cast<std::size_t>(3));
}

TEST(deleteBackwardAtCursorNotAtEndRemovesTheGraphemeBeforeIt) {
    // Cursor between 'a' and 'c' in "abc"; backspace removes 'b', not the tail.
    const auto result =
        apply({"abc", 2}, PromptTextEdit::Kind::DeleteBackward);
    ASSERT_EQ(result.text(), std::string{"ac"});
    ASSERT_EQ(result.cursor(), static_cast<std::size_t>(1));
}

TEST(deleteBackwardAtStartOfTextIsANoOp) {
    const auto result =
        apply({"abc", 0}, PromptTextEdit::Kind::DeleteBackward);
    ASSERT_EQ(result.text(), std::string{"abc"});
    ASSERT_EQ(result.cursor(), static_cast<std::size_t>(0));
}

TEST(deleteWordBackwardSkipsTrailingWhitespaceThenTheWord) {
    // Cursor at the end of "one two   "; word-backward deletes the run of
    // spaces AND the word, matching the main editor's boundary semantics.
    const auto result = apply({"one two   ", 10},
                              PromptTextEdit::Kind::DeleteWordBackward);
    ASSERT_EQ(result.text(), std::string{"one "});
    ASSERT_EQ(result.cursor(), static_cast<std::size_t>(4));
}

TEST(deleteWordBackwardAlsoConsumesTrailingPunctuationBeforeTheWord) {
    // "foo," then the cursor: the comma is a non-word byte, so the two-phase
    // skip (non-word run, then word run) consumes it AND the word "foo" in
    // one deletion -- matching isWordByte's existing precedent exactly.
    const auto result =
        apply({"foo,", 4}, PromptTextEdit::Kind::DeleteWordBackward);
    ASSERT_EQ(result.text(), std::string{""});
    ASSERT_EQ(result.cursor(), static_cast<std::size_t>(0));
}

TEST(deleteWordForwardSkipsLeadingWhitespaceThenTheWord) {
    const auto result = apply({"   two three", 0},
                              PromptTextEdit::Kind::DeleteWordForward);
    ASSERT_EQ(result.text(), std::string{" three"});
    ASSERT_EQ(result.cursor(), static_cast<std::size_t>(0));
}

TEST(deleteWordForwardAlsoConsumesLeadingPunctuationBeforeTheWord) {
    const auto result =
        apply({",foo", 0}, PromptTextEdit::Kind::DeleteWordForward);
    ASSERT_EQ(result.text(), std::string{""});
    ASSERT_EQ(result.cursor(), static_cast<std::size_t>(0));
}

TEST(multibyteGraphemeMovesAndDeletesAsOneWholeCluster) {
    // "café" -- the trailing 'é' is one two-byte UTF-8 grapheme.
    const std::string text{"caf\xc3\xa9"};
    PromptEditState state{text, text.size()};
    state = apply(state, PromptTextEdit::Kind::MoveLeft);
    ASSERT_EQ(state.cursor(), static_cast<std::size_t>(3));
    const auto deleted = apply({text, text.size()},
                               PromptTextEdit::Kind::DeleteBackward);
    ASSERT_EQ(deleted.text(), std::string{"caf"});
    ASSERT_EQ(deleted.cursor(), static_cast<std::size_t>(3));
}

TEST(multibyteGraphemeForwardDeleteRemovesTheWholeCluster) {
    const std::string text{"caf\xc3\xa9s"};  // "cafés"
    const auto result =
        apply({text, 3}, PromptTextEdit::Kind::DeleteForward);
    ASSERT_EQ(result.text(), std::string{"cafs"});
    ASSERT_EQ(result.cursor(), static_cast<std::size_t>(3));
}

TEST(insertAtAMultibyteCursorPositionStaysOnAGraphemeBoundary) {
    const std::string text{"caf\xc3\xa9"};  // "café"
    // Insert mid-string, right after the multibyte grapheme.
    const auto result =
        apply({text, text.size()}, PromptTextEdit::Kind::Insert, "!");
    ASSERT_EQ(result.text(), std::string{"caf\xc3\xa9!"});
    ASSERT_EQ(result.cursor(), text.size() + 1);
}

TEST(promptStateRejectsInvalidCursorOffsets) {
    ASSERT_THROWS(PromptEditState("abc", 4), std::invalid_argument);
    ASSERT_THROWS(PromptEditState("caf\xc3\xa9", 4), std::invalid_argument);
}

TEST(editingPreservesTheBoundaryWhenGraphemesJoin) {
    const std::string women{"\xF0\x9F\x91\xA9\xF0\x9F\x91\xA9"};
    const auto joined =
        apply({women, 4}, PromptTextEdit::Kind::Insert, "\xE2\x80\x8D");
    ASSERT_EQ(joined.cursor(), joined.text().size());
}

}  // namespace

SSG_TEST_SUITE(test_prompt_edit_state) {
    RUN(insertLandsAtTheCursorNotAlwaysAtTheEnd);
    RUN(moveLeftStepsBackOneGraphemeAndStopsAtStart);
    RUN(moveRightStepsForwardOneGraphemeAndStopsAtEnd);
    RUN(moveToStartAndMoveToEndJumpDirectlyToTheEdges);
    RUN(deleteForwardRemovesTheGraphemeAfterTheCursorAndKeepsCursorPut);
    RUN(deleteForwardAtEndOfTextIsANoOp);
    RUN(deleteBackwardAtCursorNotAtEndRemovesTheGraphemeBeforeIt);
    RUN(deleteBackwardAtStartOfTextIsANoOp);
    RUN(deleteWordBackwardSkipsTrailingWhitespaceThenTheWord);
    RUN(deleteWordBackwardAlsoConsumesTrailingPunctuationBeforeTheWord);
    RUN(deleteWordForwardSkipsLeadingWhitespaceThenTheWord);
    RUN(deleteWordForwardAlsoConsumesLeadingPunctuationBeforeTheWord);
    RUN(multibyteGraphemeMovesAndDeletesAsOneWholeCluster);
    RUN(multibyteGraphemeForwardDeleteRemovesTheWholeCluster);
    RUN(insertAtAMultibyteCursorPositionStaysOnAGraphemeBoundary);
    RUN(promptStateRejectsInvalidCursorOffsets);
    RUN(editingPreservesTheBoundaryWhenGraphemesJoin);
    return 0;
}
