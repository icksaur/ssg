#include "reference_editor.h"
#include "test_helpers.h"

#include <ssg/history.h>
#include <ssg/text_input_commands.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

ssg::DocumentPosition position(std::uint64_t offset) {
    return {ssg::ByteOffset{offset}, ssg::LineIndex{0}, ssg::CellIndex{offset}};
}

ssg::SelectionSet caret(std::uint64_t offset) {
    return ssg::SelectionSet{{ssg::Selection{position(offset), position(offset)}}};
}

ssg::SelectionSet range(std::uint64_t anchor, std::uint64_t active) {
    return ssg::SelectionSet{{
        ssg::Selection{position(anchor), position(active)},
    }};
}

ssg::HistoryResult input(ssg::DocumentHistory& history, ssg::Document& document,
                         ssg::SelectionSet& selections,
                         ssg::TextInputCommand command,
                         ssg::HistoryEditKind kind, std::uint64_t time,
                         std::string text = {}) {
    const auto generated = ssg::applyTextInput(
        document.snapshot(), selections,
        ssg::TextInputSettings{ssg::IndentStyle::Spaces, 4, false,
                               ssg::LineEnding::Lf},
        command, ssg::TextInputArguments{std::move(text)});
    ASSERT_TRUE(generated.accepted());
    if (!generated.accepted()) {
        return {ssg::HistoryError::DocumentRejected,
                ssg::DocumentError::EmptyTransaction, document.revision(),
                std::nullopt, generated.message};
    }
    auto result = history.applyEdit(document, *generated.transaction, selections,
                                     *generated.selections, kind, time);
    if (result.accepted()) {
        selections = *result.selections;
    }
    return result;
}

std::vector<ref::Sel> referenceSelections(const ssg::SelectionSet& selections) {
    std::vector<ref::Sel> result;
    for (const auto& selection : selections.items()) {
        result.push_back({static_cast<std::size_t>(
                              selection.anchor.byte_offset.value()),
                          static_cast<std::size_t>(
                              selection.active.byte_offset.value())});
    }
    return result;
}

void assertMatches(const ssg::Document& document,
                    const ssg::SelectionSet& selections,
                    const ref::Editor& reference) {
    ASSERT_EQ(document.snapshot().text, ref::snapshot_text(reference));
    ASSERT_EQ(referenceSelections(selections),
              ref::snapshot_selections(reference));
}

TEST(commandSetOwnsUndoAndRedo) {
    const auto commands = ssg::historyCommandSet();
    ASSERT_EQ(commands.descriptors()[0].id, std::string_view{"edit.undo"});
    ASSERT_EQ(commands.descriptors()[1].id, std::string_view{"edit.redo"});
}

TEST(referenceForwardUndoRedoRoundTrips) {
    ssg::Document document{"one"};
    ssg::DocumentHistory history{{4096, 750}};
    auto selections = caret(3);
    auto reference = ref::make_editor("one");

    ASSERT_TRUE(input(history, document, selections,
                      ssg::TextInputCommand::Insert,
                      ssg::HistoryEditKind::Other, 0, "!").accepted());
    ref::text_insert(reference, "!");
    assertMatches(document, selections, reference);

    auto undo = history.undo(document);
    ASSERT_TRUE(undo.accepted());
    selections = *undo.selections;
    ASSERT_TRUE(ref::edit_undo(reference));
    assertMatches(document, selections, reference);

    auto redo = history.redo(document);
    ASSERT_TRUE(redo.accepted());
    selections = *redo.selections;
    ASSERT_TRUE(ref::edit_redo(reference));
    assertMatches(document, selections, reference);

    ASSERT_TRUE(input(history, document, selections,
                      ssg::TextInputCommand::DeleteWordBackward,
                      ssg::HistoryEditKind::Other, 1000).accepted());
    ref::text_delete_word_backward(reference);
    assertMatches(document, selections, reference);
    selections = *history.undo(document).selections;
    ASSERT_TRUE(ref::edit_undo(reference));
    assertMatches(document, selections, reference);
    selections = *history.redo(document).selections;
    ASSERT_TRUE(ref::edit_redo(reference));
    assertMatches(document, selections, reference);
}

TEST(typingCoalescesAtInclusiveClockBoundary) {
    ssg::Document document;
    ssg::DocumentHistory history{{4096, 750}};
    auto selections = caret(0);

    ASSERT_TRUE(input(history, document, selections,
                      ssg::TextInputCommand::Insert,
                      ssg::HistoryEditKind::Typing, 100, "a").accepted());
    ASSERT_TRUE(input(history, document, selections,
                      ssg::TextInputCommand::Insert,
                      ssg::HistoryEditKind::Typing, 850, "b").accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"ab"});
    selections = *history.undo(document).selections;
    ASSERT_EQ(document.snapshot().text, std::string{});
    ASSERT_EQ(selections, caret(0));
    ASSERT_FALSE(history.canUndo());
    selections = *history.redo(document).selections;
    ASSERT_EQ(document.snapshot().text, std::string{"ab"});
    ASSERT_EQ(selections, caret(2));
}

TEST(multicaretTypingCoalescesAndRoundTrips) {
    ssg::Document document{"abcd"};
    ssg::DocumentHistory history{{4096, 750}};
    auto selections = ssg::SelectionSet{{
        ssg::Selection{position(1), position(1)},
        ssg::Selection{position(3), position(3)},
    }};

    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Typing, 10, "X");
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Typing, 20, "Y");
    ASSERT_EQ(document.snapshot().text, std::string{"aXYbcXYd"});
    selections = *history.undo(document).selections;
    ASSERT_EQ(document.snapshot().text, std::string{"abcd"});
    ASSERT_EQ(selections.items().size(), std::size_t{2});
    selections = *history.redo(document).selections;
    ASSERT_EQ(document.snapshot().text, std::string{"aXYbcXYd"});
}

TEST(windowKindAndBarrierSplitUnits) {
    ssg::Document document;
    ssg::DocumentHistory history{{4096, 750}};
    auto selections = caret(0);

    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Typing, 100, "a");
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Typing, 851, "b");
    selections = *history.undo(document).selections;
    ASSERT_EQ(document.snapshot().text, std::string{"a"});
    selections = *history.redo(document).selections;

    history.breakCoalescing();
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Typing, 900, "c");
    selections = *history.undo(document).selections;
    ASSERT_EQ(document.snapshot().text, std::string{"ab"});

    ssg::Document directions{"abc"};
    ssg::DocumentHistory direction_history{{4096, 750}};
    auto direction_selection = caret(1);
    input(direction_history, directions, direction_selection,
          ssg::TextInputCommand::DeleteBackward,
          ssg::HistoryEditKind::DeleteBackward, 950);
    input(direction_history, directions, direction_selection,
          ssg::TextInputCommand::DeleteForward,
          ssg::HistoryEditKind::DeleteForward, 951);
    direction_selection = *direction_history.undo(directions).selections;
    ASSERT_EQ(directions.snapshot().text, std::string{"bc"});
}

TEST(sameDirectionDeletionsCoalesce) {
    ssg::Document backward_document{"abc"};
    ssg::DocumentHistory backward_history{{4096, 750}};
    auto backward_selection = caret(3);
    input(backward_history, backward_document, backward_selection,
          ssg::TextInputCommand::DeleteBackward,
          ssg::HistoryEditKind::DeleteBackward, 10);
    input(backward_history, backward_document, backward_selection,
          ssg::TextInputCommand::DeleteBackward,
          ssg::HistoryEditKind::DeleteBackward, 20);
    backward_selection = *backward_history.undo(backward_document).selections;
    ASSERT_EQ(backward_document.snapshot().text, std::string{"abc"});
    ASSERT_EQ(backward_selection, caret(3));

    ssg::Document forward_document{"abc"};
    ssg::DocumentHistory forward_history{{4096, 750}};
    auto forward_selection = caret(0);
    input(forward_history, forward_document, forward_selection,
          ssg::TextInputCommand::DeleteForward,
          ssg::HistoryEditKind::DeleteForward, 10);
    input(forward_history, forward_document, forward_selection,
          ssg::TextInputCommand::DeleteForward,
          ssg::HistoryEditKind::DeleteForward, 20);
    forward_selection = *forward_history.undo(forward_document).selections;
    ASSERT_EQ(forward_document.snapshot().text, std::string{"abc"});
    ASSERT_EQ(forward_selection, caret(0));
}

TEST(selectionRestorationAndRedoInvalidation) {
    ssg::Document document{"abcd"};
    ssg::DocumentHistory history{{4096, 750}};
    auto selections = range(1, 3);

    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 0, "X");
    ASSERT_EQ(document.snapshot().text, std::string{"aXd"});
    ASSERT_EQ(selections, caret(2));
    selections = *history.undo(document).selections;
    ASSERT_EQ(selections, range(1, 3));
    ASSERT_EQ(document.snapshot().text, std::string{"abcd"});

    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 1, "Y");
    ASSERT_EQ(document.snapshot().text, std::string{"aYd"});
    ASSERT_FALSE(history.canRedo());
    ASSERT_EQ(history.redo(document).error, ssg::HistoryError::NoRedo);
}

TEST(byteBudgetEvictsOldestAndRejectsOversizeUnits) {
    const auto one_insert_charge =
        std::uint64_t{1} + 2 * sizeof(ssg::Selection);
    ssg::Document document;
    ssg::DocumentHistory history{{one_insert_charge, 750}};
    auto selections = caret(0);

    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 0, "a");
    ASSERT_EQ(history.retainedBytes(), one_insert_charge);
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 1, "b");
    ASSERT_EQ(history.retainedBytes(), one_insert_charge);
    selections = *history.undo(document).selections;
    ASSERT_EQ(document.snapshot().text, std::string{"a"});
    ASSERT_FALSE(history.canUndo());

    ssg::Document oversized_document;
    ssg::DocumentHistory oversized{{one_insert_charge - 1, 750}};
    auto oversized_selection = caret(0);
    input(oversized, oversized_document, oversized_selection,
          ssg::TextInputCommand::Insert, ssg::HistoryEditKind::Other, 0, "x");
    ASSERT_FALSE(oversized.canUndo());
    ASSERT_EQ(oversized.retainedBytes(), std::uint64_t{0});

    ssg::Document disabled_document;
    ssg::DocumentHistory disabled{{0, 750}};
    auto disabled_selection = caret(0);
    input(disabled, disabled_document, disabled_selection,
          ssg::TextInputCommand::Insert, ssg::HistoryEditKind::Other, 0, "x");
    ASSERT_FALSE(disabled.canUndo());
}

TEST(rejectionAndStaleDocumentAreFailureAtomic) {
    ssg::Document document{"a"};
    ssg::DocumentHistory history{{4096, 750}};
    auto selections = caret(1);
    const auto stale = ssg::EditTransaction{
        ssg::Revision{99}, {{ssg::ByteOffset{1}, 0, "b"}}};
    const auto rejected =
        history.applyEdit(document, stale, selections, caret(2),
                           ssg::HistoryEditKind::Other, 0);
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.document_error, ssg::DocumentError::StaleRevision);
    ASSERT_EQ(document.snapshot().text, std::string{"a"});
    ASSERT_FALSE(history.canUndo());

    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 1, "b");
    const auto direct = document.apply(
        {document.revision(), {{ssg::ByteOffset{2}, 0, "c"}}});
    ASSERT_TRUE(direct.accepted());
    const auto before = document.snapshot();
    const auto undo = history.undo(document);
    ASSERT_EQ(undo.error, ssg::HistoryError::StaleDocument);
    ASSERT_EQ(document.snapshot(), before);
    ASSERT_TRUE(history.canUndo());
}

TEST(undoRedoAdvanceRevisionAndKeepDirty) {
    ssg::Document document;
    ssg::DocumentHistory history{{4096, 750}};
    auto selections = caret(0);
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 0, "a");
    const auto after_edit = document.revision();
    selections = *history.undo(document).selections;
    ASSERT_TRUE(document.revision() > after_edit);
    ASSERT_TRUE(document.dirty());
    const auto after_undo = document.revision();
    selections = *history.redo(document).selections;
    ASSERT_TRUE(document.revision() > after_undo);
    ASSERT_TRUE(document.dirty());
}

TEST(viewStateAndDeltaTrackHistoryAvailability) {
    ssg::Document document;
    ssg::DocumentHistory history{{4096, 750}};
    const auto empty = history.viewState();
    ASSERT_FALSE(empty.can_undo);
    ASSERT_FALSE(empty.can_redo);
    ASSERT_FALSE(ssg::deriveHistoryDelta(empty, empty).changed);

    auto selections = caret(0);
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 0, "a");
    const auto edited = history.viewState();
    const auto delta = ssg::deriveHistoryDelta(empty, edited);
    ASSERT_TRUE(delta.changed);
    ASSERT_EQ(*delta.replacement, edited);
    ASSERT_TRUE(edited.can_undo);

    selections = *history.undo(document).selections;
    const auto undone = history.viewState();
    ASSERT_FALSE(undone.can_undo);
    ASSERT_TRUE(undone.can_redo);
}

}  // namespace

int main() {
    RUN(commandSetOwnsUndoAndRedo);
    RUN(referenceForwardUndoRedoRoundTrips);
    RUN(typingCoalescesAtInclusiveClockBoundary);
    RUN(multicaretTypingCoalescesAndRoundTrips);
    RUN(windowKindAndBarrierSplitUnits);
    RUN(sameDirectionDeletionsCoalesce);
    RUN(selectionRestorationAndRedoInvalidation);
    RUN(byteBudgetEvictsOldestAndRejectsOversizeUnits);
    RUN(rejectionAndStaleDocumentAreFailureAtomic);
    RUN(undoRedoAdvanceRevisionAndKeepDirty);
    RUN(viewStateAndDeltaTrackHistoryAvailability);
    return failed == 0 ? 0 : 1;
}
