#include "reference_editor.h"
#include "test_helpers.h"

#include <ssg/DocumentHistory.h>
#include <ssg/TextInputCommands.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct HistoryFixture {
    explicit HistoryFixture(
        std::uint64_t byteBudget = 4096,
        std::uint32_t coalescingMs = 750)
        : history{settings} {
        ASSERT_TRUE(settings
                        .set(ssg::SettingScope::User,
                             ssg::SettingKey::UndoByteBudget, byteBudget)
                        .accepted());
        ASSERT_TRUE(settings
                        .set(ssg::SettingScope::User,
                             ssg::SettingKey::TypingCoalescingMs, coalescingMs)
                        .accepted());
    }

    ssg::SettingsModel settings;
    ssg::DocumentHistory history;
};

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
    const auto generated = ssg::TextInputInterpreter{}.apply(
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
                              selection.anchor.byteOffset.value()),
                          static_cast<std::size_t>(
                              selection.active.byteOffset.value())});
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

TEST(referenceForwardUndoRedoRoundTrips) {
    ssg::Document document{"one"};
    HistoryFixture fixture;
    auto& history = fixture.history;
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
    HistoryFixture fixture;
    auto& history = fixture.history;
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

TEST(settingChangesApplyToTheNextEdit) {
    ssg::Document document;
    HistoryFixture fixture;
    auto& history = fixture.history;
    auto selections = caret(0);

    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Typing, 0, "a");
    ASSERT_TRUE(fixture.settings
                    .set(ssg::SettingScope::User,
                         ssg::SettingKey::TypingCoalescingMs,
                         std::uint32_t{0})
                    .accepted());
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Typing, 1, "b");
    selections = *history.undo(document).selections;
    ASSERT_EQ(document.snapshot().text, std::string{"a"});

    ASSERT_TRUE(fixture.settings
                    .set(ssg::SettingScope::User,
                         ssg::SettingKey::UndoByteBudget,
                         std::uint64_t{0})
                    .accepted());
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 2, "c");
    ASSERT_FALSE(history.canUndo());
}

TEST(multicaretTypingCoalescesAndRoundTrips) {
    ssg::Document document{"abcd"};
    HistoryFixture fixture;
    auto& history = fixture.history;
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
    HistoryFixture fixture;
    auto& history = fixture.history;
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
    HistoryFixture directionFixture;
    auto& directionHistory = directionFixture.history;
    auto directionSelection = caret(1);
    input(directionHistory, directions, directionSelection,
          ssg::TextInputCommand::DeleteBackward,
          ssg::HistoryEditKind::DeleteBackward, 950);
    input(directionHistory, directions, directionSelection,
          ssg::TextInputCommand::DeleteForward,
          ssg::HistoryEditKind::DeleteForward, 951);
    directionSelection = *directionHistory.undo(directions).selections;
    ASSERT_EQ(directions.snapshot().text, std::string{"bc"});
}

TEST(sameDirectionDeletionsCoalesce) {
    ssg::Document backwardDocument{"abc"};
    HistoryFixture backwardFixture;
    auto& backwardHistory = backwardFixture.history;
    auto backwardSelection = caret(3);
    input(backwardHistory, backwardDocument, backwardSelection,
          ssg::TextInputCommand::DeleteBackward,
          ssg::HistoryEditKind::DeleteBackward, 10);
    input(backwardHistory, backwardDocument, backwardSelection,
          ssg::TextInputCommand::DeleteBackward,
          ssg::HistoryEditKind::DeleteBackward, 20);
    backwardSelection = *backwardHistory.undo(backwardDocument).selections;
    ASSERT_EQ(backwardDocument.snapshot().text, std::string{"abc"});
    ASSERT_EQ(backwardSelection, caret(3));

    ssg::Document forwardDocument{"abc"};
    HistoryFixture forwardFixture;
    auto& forwardHistory = forwardFixture.history;
    auto forwardSelection = caret(0);
    input(forwardHistory, forwardDocument, forwardSelection,
          ssg::TextInputCommand::DeleteForward,
          ssg::HistoryEditKind::DeleteForward, 10);
    input(forwardHistory, forwardDocument, forwardSelection,
          ssg::TextInputCommand::DeleteForward,
          ssg::HistoryEditKind::DeleteForward, 20);
    forwardSelection = *forwardHistory.undo(forwardDocument).selections;
    ASSERT_EQ(forwardDocument.snapshot().text, std::string{"abc"});
    ASSERT_EQ(forwardSelection, caret(0));
}

TEST(selectionRestorationAndRedoInvalidation) {
    ssg::Document document{"abcd"};
    HistoryFixture fixture;
    auto& history = fixture.history;
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
    const auto oneInsertCharge =
        std::uint64_t{1} + 2 * sizeof(ssg::Selection);
    ssg::Document document;
    HistoryFixture fixture{oneInsertCharge};
    auto& history = fixture.history;
    auto selections = caret(0);

    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 0, "a");
    ASSERT_EQ(history.retainedBytes(), oneInsertCharge);
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 1, "b");
    ASSERT_EQ(history.retainedBytes(), oneInsertCharge);
    selections = *history.undo(document).selections;
    ASSERT_EQ(document.snapshot().text, std::string{"a"});
    ASSERT_FALSE(history.canUndo());

    ssg::Document oversizedDocument;
    HistoryFixture oversizedFixture{oneInsertCharge - 1};
    auto& oversized = oversizedFixture.history;
    auto oversizedSelection = caret(0);
    input(oversized, oversizedDocument, oversizedSelection,
          ssg::TextInputCommand::Insert, ssg::HistoryEditKind::Other, 0, "x");
    ASSERT_FALSE(oversized.canUndo());
    ASSERT_EQ(oversized.retainedBytes(), std::uint64_t{0});

    ssg::Document disabledDocument;
    HistoryFixture disabledFixture{0};
    auto& disabled = disabledFixture.history;
    auto disabledSelection = caret(0);
    input(disabled, disabledDocument, disabledSelection,
          ssg::TextInputCommand::Insert, ssg::HistoryEditKind::Other, 0, "x");
    ASSERT_FALSE(disabled.canUndo());
}

TEST(rejectionAndStaleDocumentAreFailureAtomic) {
    ssg::Document document{"a"};
    HistoryFixture fixture;
    auto& history = fixture.history;
    auto selections = caret(1);
    const auto stale = ssg::EditTransaction{
        std::uint64_t{99}, {{ssg::ByteOffset{1}, 0, "b"}}};
    const auto rejected =
        history.applyEdit(document, stale, selections, caret(2),
                           ssg::HistoryEditKind::Other, 0);
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.documentError, ssg::DocumentError::StaleRevision);
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
    HistoryFixture fixture;
    auto& history = fixture.history;
    auto selections = caret(0);
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 0, "a");
    const auto afterEdit = document.revision();
    selections = *history.undo(document).selections;
    ASSERT_TRUE(document.revision() > afterEdit);
    ASSERT_TRUE(document.dirty());
    const auto afterUndo = document.revision();
    selections = *history.redo(document).selections;
    ASSERT_TRUE(document.revision() > afterUndo);
    ASSERT_TRUE(document.dirty());
}

TEST(viewStateTracksHistoryAvailability) {
    ssg::Document document;
    HistoryFixture fixture;
    auto& history = fixture.history;
    const auto empty = history.viewState();
    ASSERT_FALSE(empty.canUndo);
    ASSERT_FALSE(empty.canRedo);

    auto selections = caret(0);
    input(history, document, selections, ssg::TextInputCommand::Insert,
          ssg::HistoryEditKind::Other, 0, "a");
    const auto edited = history.viewState();
    ASSERT_TRUE(edited.canUndo);

    selections = *history.undo(document).selections;
    const auto undone = history.viewState();
    ASSERT_FALSE(undone.canUndo);
    ASSERT_TRUE(undone.canRedo);
}

}  // namespace

SSG_TEST_SUITE(test_history) {
    RUN(referenceForwardUndoRedoRoundTrips);
    RUN(typingCoalescesAtInclusiveClockBoundary);
    RUN(settingChangesApplyToTheNextEdit);
    RUN(multicaretTypingCoalescesAndRoundTrips);
    RUN(windowKindAndBarrierSplitUnits);
    RUN(sameDirectionDeletionsCoalesce);
    RUN(selectionRestorationAndRedoInvalidation);
    RUN(byteBudgetEvictsOldestAndRejectsOversizeUnits);
    RUN(rejectionAndStaleDocumentAreFailureAtomic);
    RUN(undoRedoAdvanceRevisionAndKeepDirty);
    RUN(viewStateTracksHistoryAvailability);
    return failed == 0 ? 0 : 1;
}
