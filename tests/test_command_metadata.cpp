#include <ssg/command_metadata.h>
#include <ssg/keymap.h>

#include "test_helpers.h"

#include <string>

TEST(commandLabelUsesAuthoredLabelsAndHumanizesTheRest) {
    // Authored labels for common commands.
    ASSERT_EQ(ssg::commandLabel("file.save"), std::string{"Save File"});
    ASSERT_EQ(ssg::commandLabel("edit.undo"), std::string{"Undo"});
    ASSERT_EQ(ssg::commandLabel("palette.open"),
              std::string{"Command Palette"});

    // Uncurated ids humanize from their segments, never showing the raw id.
    ASSERT_EQ(ssg::commandLabel("cursor.line_down"),
              std::string{"Cursor Line Down"});
    ASSERT_NE(ssg::commandLabel("cursor.line_down"),
              std::string{"cursor.line_down"});
    ASSERT_EQ(ssg::commandLabel("tree.select_previous"),
              std::string{"Tree Select Previous"});
}

TEST(formatKeySequenceIsCompactAndHuman) {
    ASSERT_EQ(ssg::KeyCodec{}.formatSequence(*ssg::KeyCodec{}.parseSequence({"Escape", "KeyS"})),
              std::string{"Esc S"});
    ASSERT_EQ(ssg::KeyCodec{}.formatSequence(*ssg::KeyCodec{}.parseSequence({"ArrowDown"})),
              std::string{"Down"});
    ASSERT_EQ(ssg::KeyCodec{}.formatSequence(
                  *ssg::KeyCodec{}.parseSequence({"Escape", "Shift+KeyZ"})),
              std::string{"Esc Shift+Z"});
    ASSERT_EQ(ssg::KeyCodec{}.formatSequence(
                  *ssg::KeyCodec{}.parseSequence({"Escape", "BracketRight"})),
              std::string{"Esc ]"});
    ASSERT_TRUE(ssg::KeyCodec{}.formatSequence({}).empty());
}

TEST(preferredBindingIsDeterministic) {
    const auto shortSeq = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyS"});
    const auto longSeq = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyF", "KeyT"});
    // Two bindings for one command: the shorter wins regardless of order.
    ssg::KeymapViewState a{"m", {{longSeq, "cmd", "*"}, {shortSeq, "cmd", "*"}}};
    ssg::KeymapViewState b{"m", {{shortSeq, "cmd", "*"}, {longSeq, "cmd", "*"}}};
    auto aPref = ssg::KeymapMatcher{a}.preferredBinding("cmd");
    auto bPref = ssg::KeymapMatcher{b}.preferredBinding("cmd");
    ASSERT_TRUE(aPref.has_value());
    ASSERT_TRUE(bPref.has_value());
    ASSERT_EQ(*aPref, shortSeq);
    ASSERT_EQ(*bPref, shortSeq);

    // Equal length: the lexicographically least display form wins.
    const auto escA = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyA"});
    const auto escB = *ssg::KeyCodec{}.parseSequence({"Escape", "KeyB"});
    ssg::KeymapViewState c{"m", {{escB, "cmd", "*"}, {escA, "cmd", "*"}}};
    auto cPref = ssg::KeymapMatcher{c}.preferredBinding("cmd");
    ASSERT_TRUE(cPref.has_value());
    ASSERT_EQ(*cPref, escA);

    // Unbound command -> no preferred binding.
    ASSERT_FALSE(ssg::KeymapMatcher{a}.preferredBinding("other").has_value());
}

int main() {
    RUN(commandLabelUsesAuthoredLabelsAndHumanizesTheRest);
    RUN(formatKeySequenceIsCompactAndHuman);
    RUN(preferredBindingIsDeterministic);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
