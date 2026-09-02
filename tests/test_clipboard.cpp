#include "test_helpers.h"

#include <ssg/ClipboardRegister.h>

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
    const auto state = clipboard.viewState();
    ASSERT_EQ(state.fragments,
              (std::vector<std::string>{"aa", "bb\r", "last"}));
    ASSERT_EQ(state.plainText, std::string{"aabb\rlast"});
    ASSERT_EQ(copied.write->text, state.plainText);
    ASSERT_EQ(state.systemWrite, copied.write);
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
        distributed, distributedHistory, distributedBefore, 10);
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
        fallback, fallbackHistory, fallbackBefore, 20);
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
    ASSERT_EQ(cut.write->text, std::string{"two\ntwo\n"});
    ASSERT_EQ(clipboard.viewState().systemWrite, cut.write);
    const auto undo = history.undo(document);
    ASSERT_TRUE(undo.accepted());
    ASSERT_EQ(document.snapshot().text, text);
    ASSERT_EQ(*undo.selections, before);
    ASSERT_TRUE(history.redo(document).accepted());
    ASSERT_EQ(document.snapshot().text, std::string{"one\r\nlast"});
}

TEST(nonEditModesAreAtomic) {
    ssg::ClipboardRegister clipboard;
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
            blocked, blockedHistory, blockedBefore, 40);
        ASSERT_FALSE(paste.accepted());
        ASSERT_EQ(blocked.snapshot().text, std::string{"x"});
    }
}

TEST(everyCopyPublishesAStrictlyNewerSystemWriteId) {
    ssg::ClipboardRegister clipboard;
    ssg::Document document{"ab"};

    const auto first = clipboard.copy(document.snapshot(), selections("ab", {{0, 1}}));
    ASSERT_TRUE(first.accepted());
    const auto firstId = clipboard.viewState().systemWrite->id;
    ASSERT_EQ(firstId, first.write->id);

    const auto second = clipboard.copy(document.snapshot(), selections("ab", {{1, 2}}));
    ASSERT_TRUE(second.accepted());
    const auto published = clipboard.viewState().systemWrite;
    ASSERT_TRUE(published->id > firstId);
    ASSERT_EQ(published->id, second.write->id);
    ASSERT_EQ(published->text, std::string{"b"});
}

TEST(viewDeltaReportsRegisterAndRequestChanges) {
    ssg::ClipboardRegister clipboard;
    const auto empty = clipboard.viewState();
    ASSERT_FALSE(ssg::ClipboardDeltaCodec{}.derive(empty, empty).changed);

    ssg::Document document{"a"};
    const auto copyResult =
        clipboard.copy(document.snapshot(), selections("a", {{0, 1}}));
    ASSERT_TRUE(copyResult.accepted());
    const auto copiedState = clipboard.viewState();
    const auto delta = ssg::ClipboardDeltaCodec{}.derive(empty, copiedState);
    ASSERT_TRUE(delta.changed);
    ASSERT_EQ(*delta.replacement, copiedState);
}

}  // namespace

SSG_TEST_SUITE(test_clipboard) {
    RUN(commandSetOwnsClipboardCommands);
    RUN(copyCapturesFragmentsLinesAndExactPlainPayload);
    RUN(lineCopyPreservesDuplicatesAndEmptyFinalLine);
    RUN(fragmentDistributionAndPlainPayloadFallbackRoundTrip);
    RUN(lineCutMergesDuplicateRangesAndIsUndoable);
    RUN(nonEditModesAreAtomic);
    RUN(everyCopyPublishesAStrictlyNewerSystemWriteId);
    RUN(viewDeltaReportsRegisterAndRequestChanges);
    return failed == 0 ? 0 : 1;
}
