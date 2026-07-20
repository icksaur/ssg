#include "test_helpers.h"

#include <ssg/clipboard.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

ssg::DocumentPosition position(std::string_view text, std::uint64_t offset) {
    return *ssg::SelectionNavigator::resolvePosition(text, ssg::ByteOffset{offset}, 4);
}

ssg::Selection selection(std::string_view text, std::uint64_t anchor,
                         std::uint64_t active) {
    return {position(text, anchor), position(text, active)};
}

ssg::SelectionSet selections(
    std::string_view text,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> ranges) {
    std::vector<ssg::Selection> result;
    for (const auto [anchor, active] : ranges) {
        result.push_back(selection(text, anchor, active));
    }
    return ssg::SelectionSet{std::move(result)};
}

ssg::ClipboardResponse responseFor(
    const ssg::ClipboardRequest& request, ssg::ClipboardResponseStatus status,
    std::string text = {}) {
    return {request.id, request.requestRevision, request.requestRevision,
            status, std::move(text)};
}

TEST(commandSetOwnsClipboardCommands) {
    const auto commands = ssg::clipboardCommandSet();
    ASSERT_EQ(commands.descriptors()[0].id,
              std::string_view{"clipboard.copy"});
    ASSERT_EQ(commands.descriptors()[1].id,
              std::string_view{"clipboard.cut"});
    ASSERT_EQ(commands.descriptors()[2].id,
              std::string_view{"clipboard.paste"});
}

TEST(copyCapturesFragmentsLinesAndExactPlainPayload) {
    const std::string text = "aa\r\nbb\rc\nlast";
    ssg::Document document{text};
    ssg::ClipboardRegister clipboard;
    const auto selected = selections(text, {{0, 2}, {5, 5}, {10, 10}});

    const auto copied = clipboard.copy(document.snapshot(), selected);

    ASSERT_TRUE(copied.accepted());
    ASSERT_FALSE(copied.documentChanged);
    ASSERT_EQ(copied.systemStatus, ssg::ClipboardSystemStatus::Pending);
    ASSERT_EQ(copied.request->kind, ssg::ClipboardRequestKind::Write);
    const auto state = clipboard.viewState();
    ASSERT_EQ(state.fragments,
              (std::vector<std::string>{"aa", "bb\r", "last"}));
    ASSERT_EQ(state.plainText, std::string{"aabb\rlast"});
    ASSERT_EQ(copied.request->text, state.plainText);
    ASSERT_EQ(document.snapshot().text, text);
}

TEST(lineCopyPreservesDuplicatesAndEmptyFinalLine) {
    const std::string text = "one\n";
    ssg::Document document{text};
    ssg::ClipboardRegister clipboard;

    const auto copied = clipboard.copy(
        document.snapshot(), selections(text, {{1, 1}, {2, 2}, {4, 4}}));

    ASSERT_TRUE(copied.accepted());
    ASSERT_EQ(clipboard.viewState().fragments,
              (std::vector<std::string>{"one\n", "one\n", ""}));
    ASSERT_EQ(clipboard.viewState().plainText, std::string{"one\none\n"});
}

TEST(fragmentDistributionAndPlainPayloadFallbackRoundTrip) {
    ssg::ClipboardRegister clipboard;
    ssg::Document source{"AB"};
    ASSERT_TRUE(clipboard
                    .copy(source.snapshot(),
                          selections("AB", {{0, 1}, {1, 2}}))
                    .accepted());

    ssg::Document distributed{"xx"};
    ssg::DocumentHistory distributedHistory;
    const auto distributedBefore = selections("xx", {{0, 0}, {2, 2}});
    const auto paste = clipboard.paste(
        distributed, distributedHistory, distributedBefore,
        ssg::ClipboardPasteMode::InternalOnly, 10);
    ASSERT_TRUE(paste.accepted());
    ASSERT_EQ(distributed.snapshot().text, std::string{"AxxB"});
    ASSERT_TRUE(paste.documentChanged);
    auto undone = distributedHistory.undo(distributed);
    ASSERT_TRUE(undone.accepted());
    ASSERT_EQ(distributed.snapshot().text, std::string{"xx"});
    ASSERT_EQ(*undone.selections, distributedBefore);
    auto redone = distributedHistory.redo(distributed);
    ASSERT_TRUE(redone.accepted());
    ASSERT_EQ(distributed.snapshot().text, std::string{"AxxB"});

    ssg::Document fallback{"xyz"};
    ssg::DocumentHistory fallbackHistory;
    const auto fallbackBefore = selections("xyz", {{0, 0}, {1, 1}, {3, 3}});
    const auto fallbackPaste = clipboard.paste(
        fallback, fallbackHistory, fallbackBefore,
        ssg::ClipboardPasteMode::InternalOnly, 20);
    ASSERT_TRUE(fallbackPaste.accepted());
    ASSERT_EQ(fallback.snapshot().text, std::string{"ABxAByzAB"});
    const auto fallbackUndo = fallbackHistory.undo(fallback);
    ASSERT_TRUE(fallbackUndo.accepted());
    ASSERT_EQ(*fallbackUndo.selections, fallbackBefore);
    ASSERT_EQ(fallback.snapshot().text, std::string{"xyz"});
}

TEST(lineCutMergesDuplicateRangesAndIsUndoable) {
    const std::string text = "one\r\ntwo\nlast";
    ssg::Document document{text};
    ssg::DocumentHistory history;
    ssg::ClipboardRegister clipboard;
    const auto before = selections(text, {{6, 6}, {7, 7}});

    const auto cut = clipboard.cut(document, history, before, 10);

    ASSERT_TRUE(cut.accepted());
    ASSERT_TRUE(cut.documentChanged);
    ASSERT_EQ(document.snapshot().text, std::string{"one\r\nlast"});
    ASSERT_EQ(clipboard.viewState().fragments,
              (std::vector<std::string>{"two\n", "two\n"}));
    ASSERT_EQ(cut.request->text, std::string{"two\ntwo\n"});
    const auto undo = history.undo(document);
    ASSERT_TRUE(undo.accepted());
    ASSERT_EQ(document.snapshot().text, text);
    ASSERT_EQ(*undo.selections, before);
    ASSERT_TRUE(history.redo(document).accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"one\r\nlast"});
}

void assertFallbackResponse(ssg::ClipboardResponseStatus status,
                              ssg::ClipboardSystemStatus expectedStatus) {
    ssg::ClipboardRegister clipboard;
    ssg::Document source{"fallback"};
    ASSERT_TRUE(clipboard
                    .copy(source.snapshot(), selections("fallback", {{0, 8}}))
                    .accepted());
    ssg::Document target{"x"};
    ssg::DocumentHistory history;
    const auto before = selections("x", {{1, 1}});
    const auto pending = clipboard.paste(
        target, history, before, ssg::ClipboardPasteMode::SystemFirst, 10);
    ASSERT_TRUE(pending.accepted());
    ASSERT_FALSE(pending.documentChanged);
    ASSERT_EQ(pending.request->kind, ssg::ClipboardRequestKind::Read);

    const auto handled = clipboard.handleResponse(
        target, history, before, responseFor(*pending.request, status), 20);

    ASSERT_TRUE(handled.accepted());
    ASSERT_EQ(handled.systemStatus, expectedStatus);
    ASSERT_EQ(target.snapshot().text, std::string{"xfallback"});
    ASSERT_TRUE(history.undo(target).accepted());
    ASSERT_EQ(target.snapshot().text, std::string{"x"});
}

TEST(deniedUnavailableAndDisconnectedReadsUseInternalFallback) {
    assertFallbackResponse(ssg::ClipboardResponseStatus::Denied,
                             ssg::ClipboardSystemStatus::Denied);
    assertFallbackResponse(ssg::ClipboardResponseStatus::Unavailable,
                             ssg::ClipboardSystemStatus::Unavailable);
    assertFallbackResponse(ssg::ClipboardResponseStatus::Disconnected,
                             ssg::ClipboardSystemStatus::Disconnected);
}

TEST(successfulSystemReadIsOneUndoablePaste) {
    ssg::ClipboardRegister clipboard;
    ssg::Document target{"ac"};
    ssg::DocumentHistory history;
    const auto before = selections("ac", {{1, 1}});
    const auto pending = clipboard.paste(
        target, history, before, ssg::ClipboardPasteMode::SystemFirst, 10);

    const auto handled = clipboard.handleResponse(
        target, history, before,
        responseFor(*pending.request, ssg::ClipboardResponseStatus::Success,
                     "B"),
        20);

    ASSERT_TRUE(handled.accepted());
    ASSERT_EQ(handled.systemStatus, ssg::ClipboardSystemStatus::Succeeded);
    ASSERT_EQ(target.snapshot().text, std::string{"aBc"});
    ASSERT_TRUE(history.undo(target).accepted());
    ASSERT_EQ(target.snapshot().text, std::string{"ac"});
    const auto redo = history.redo(target);
    ASSERT_TRUE(redo.accepted());
    ASSERT_EQ(*redo.selections, *handled.selections);
    ASSERT_EQ(target.snapshot().text, std::string{"aBc"});
}

TEST(staleReadNeverAppliesSystemTextOrFallback) {
    ssg::ClipboardRegister clipboard;
    ssg::Document source{"fallback"};
    const auto copied =
        clipboard.copy(source.snapshot(), selections("fallback", {{0, 8}}));
    ASSERT_TRUE(copied.accepted());
    ssg::Document target{"x"};
    ssg::DocumentHistory history;
    const auto before = selections("x", {{1, 1}});
    const auto pending = clipboard.paste(
        target, history, before, ssg::ClipboardPasteMode::SystemFirst, 10);
    ASSERT_TRUE(target.apply({target.revision(), {{ssg::ByteOffset{1}, 0, "!"}}})
                    .accepted());
    const auto snapshot = target.snapshot();

    const auto stale = clipboard.handleResponse(
        target, history, before,
        responseFor(*pending.request, ssg::ClipboardResponseStatus::Denied),
        20);

    ASSERT_FALSE(stale.accepted());
    ASSERT_EQ(stale.error, ssg::ClipboardError::StaleResponse);
    ASSERT_EQ(stale.systemStatus, ssg::ClipboardSystemStatus::Stale);
    ASSERT_EQ(target.snapshot(), snapshot);
    ASSERT_FALSE(history.canUndo());
}

TEST(changedSelectionMakesReadResponseStale) {
    ssg::ClipboardRegister clipboard;
    ssg::Document target{"ab"};
    ssg::DocumentHistory history;
    const auto requested = selections("ab", {{1, 1}});
    const auto pending = clipboard.paste(
        target, history, requested, ssg::ClipboardPasteMode::SystemFirst, 10);
    const auto moved = selections("ab", {{2, 2}});

    const auto stale = clipboard.handleResponse(
        target, history, moved,
        responseFor(*pending.request, ssg::ClipboardResponseStatus::Success,
                     "X"),
        20);

    ASSERT_EQ(stale.error, ssg::ClipboardError::StaleResponse);
    ASSERT_EQ(target.snapshot().text, std::string{"ab"});
}

TEST(invalidSystemTextAndNonEditModesAreAtomic) {
    ssg::ClipboardRegister clipboard;
    ssg::Document target{"x"};
    ssg::DocumentHistory history;
    const auto before = selections("x", {{1, 1}});
    const auto pending = clipboard.paste(
        target, history, before, ssg::ClipboardPasteMode::SystemFirst, 10);
    const std::string invalid{"\xC3", 1};
    const auto rejected = clipboard.handleResponse(
        target, history, before,
        responseFor(*pending.request, ssg::ClipboardResponseStatus::Success,
                     invalid),
        20);
    ASSERT_EQ(rejected.error, ssg::ClipboardError::InvalidUtf8);
    ASSERT_EQ(target.snapshot().text, std::string{"x"});
    ASSERT_FALSE(history.canUndo());

    for (const auto mode :
         {ssg::DocumentMode::ReadOnly, ssg::DocumentMode::Diff}) {
        ssg::Document blocked{"x", mode};
        ssg::DocumentHistory blockedHistory;
        const auto blockedBefore = selections("x", {{0, 1}});
        const auto cut =
            clipboard.cut(blocked, blockedHistory, blockedBefore, 30);
        ASSERT_FALSE(cut.accepted());
        ASSERT_EQ(blocked.snapshot().text, std::string{"x"});
        const auto paste = clipboard.paste(
            blocked, blockedHistory, blockedBefore,
            ssg::ClipboardPasteMode::InternalOnly, 40);
        ASSERT_FALSE(paste.accepted());
        ASSERT_EQ(blocked.snapshot().text, std::string{"x"});
    }
}

TEST(writeFailureDoesNotRollBackCopyOrCut) {
    ssg::ClipboardRegister clipboard;
    ssg::Document document{"line\n"};
    ssg::DocumentHistory history;
    const auto before = selections("line\n", {{0, 0}});
    const auto cut = clipboard.cut(document, history, before, 10);
    const auto stateAfterCut = clipboard.viewState();
    const auto textAfterCut = document.snapshot().text;

    const auto failure = clipboard.handleResponse(
        document, history, *cut.selections,
        responseFor(*cut.request, ssg::ClipboardResponseStatus::Denied), 20);

    ASSERT_TRUE(failure.accepted());
    ASSERT_EQ(failure.systemStatus, ssg::ClipboardSystemStatus::Denied);
    ASSERT_EQ(clipboard.viewState().fragments, stateAfterCut.fragments);
    ASSERT_EQ(document.snapshot().text, textAfterCut);
    ASSERT_TRUE(history.canUndo());
}

TEST(writeFailureReportsStatusAfterDocumentAdvances) {
    ssg::ClipboardRegister clipboard;
    ssg::Document document{"a"};
    ssg::DocumentHistory history;
    const auto copied =
        clipboard.copy(document.snapshot(), selections("a", {{0, 1}}));
    ASSERT_TRUE(copied.accepted());
    ASSERT_TRUE(document
                    .apply({document.revision(),
                            {{ssg::ByteOffset{1}, 0, "b"}}})
                    .accepted());
    const auto current = selections("ab", {{2, 2}});

    const auto failure = clipboard.handleResponse(
        document, history, current,
        responseFor(*copied.request, ssg::ClipboardResponseStatus::Denied),
        20);

    ASSERT_TRUE(failure.accepted());
    ASSERT_EQ(failure.systemStatus, ssg::ClipboardSystemStatus::Denied);
    ASSERT_EQ(document.snapshot().text, std::string{"ab"});
    ASSERT_EQ(clipboard.viewState().plainText, std::string{"a"});
}

TEST(viewDeltaReportsRegisterAndRequestChanges) {
    ssg::ClipboardRegister clipboard;
    const auto empty = clipboard.viewState();
    ASSERT_FALSE(ssg::deriveClipboardDelta(empty, empty).changed);

    ssg::Document document{"a"};
    const auto copyResult =
        clipboard.copy(document.snapshot(), selections("a", {{0, 1}}));
    ASSERT_TRUE(copyResult.accepted());
    const auto copiedState = clipboard.viewState();
    const auto delta = ssg::deriveClipboardDelta(empty, copiedState);
    ASSERT_TRUE(delta.changed);
    ASSERT_EQ(*delta.replacement, copiedState);
}

}  // namespace

int main() {
    RUN(commandSetOwnsClipboardCommands);
    RUN(copyCapturesFragmentsLinesAndExactPlainPayload);
    RUN(lineCopyPreservesDuplicatesAndEmptyFinalLine);
    RUN(fragmentDistributionAndPlainPayloadFallbackRoundTrip);
    RUN(lineCutMergesDuplicateRangesAndIsUndoable);
    RUN(deniedUnavailableAndDisconnectedReadsUseInternalFallback);
    RUN(successfulSystemReadIsOneUndoablePaste);
    RUN(staleReadNeverAppliesSystemTextOrFallback);
    RUN(changedSelectionMakesReadResponseStale);
    RUN(invalidSystemTextAndNonEditModesAreAtomic);
    RUN(writeFailureDoesNotRollBackCopyOrCut);
    RUN(writeFailureReportsStatusAfterDocumentAdvances);
    RUN(viewDeltaReportsRegisterAndRequestChanges);
    return failed == 0 ? 0 : 1;
}
