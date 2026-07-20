#include <ssg/command_metadata.h>
#include <ssg/input.h>

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
    ASSERT_EQ(ssg::formatKeySequence(*ssg::parseKeySequence({"Escape", "KeyS"})),
              std::string{"Esc S"});
    ASSERT_EQ(ssg::formatKeySequence(*ssg::parseKeySequence({"ArrowDown"})),
              std::string{"Down"});
    ASSERT_EQ(ssg::formatKeySequence(
                  *ssg::parseKeySequence({"Escape", "Shift+KeyZ"})),
              std::string{"Esc Shift+Z"});
    ASSERT_EQ(ssg::formatKeySequence(
                  *ssg::parseKeySequence({"Escape", "BracketRight"})),
              std::string{"Esc ]"});
    ASSERT_TRUE(ssg::formatKeySequence({}).empty());
}

TEST(preferredBindingIsDeterministic) {
    const auto short_seq = *ssg::parseKeySequence({"Escape", "KeyS"});
    const auto long_seq = *ssg::parseKeySequence({"Escape", "KeyF", "KeyT"});
    // Two bindings for one command: the shorter wins regardless of order.
    ssg::KeymapViewState a{"m", {{long_seq, "cmd", "*"}, {short_seq, "cmd", "*"}}};
    ssg::KeymapViewState b{"m", {{short_seq, "cmd", "*"}, {long_seq, "cmd", "*"}}};
    auto a_pref = ssg::preferredBinding(a, "cmd");
    auto b_pref = ssg::preferredBinding(b, "cmd");
    ASSERT_TRUE(a_pref.has_value());
    ASSERT_TRUE(b_pref.has_value());
    ASSERT_EQ(*a_pref, short_seq);
    ASSERT_EQ(*b_pref, short_seq);

    // Equal length: the lexicographically least display form wins.
    const auto esc_a = *ssg::parseKeySequence({"Escape", "KeyA"});
    const auto esc_b = *ssg::parseKeySequence({"Escape", "KeyB"});
    ssg::KeymapViewState c{"m", {{esc_b, "cmd", "*"}, {esc_a, "cmd", "*"}}};
    auto c_pref = ssg::preferredBinding(c, "cmd");
    ASSERT_TRUE(c_pref.has_value());
    ASSERT_EQ(*c_pref, esc_a);

    // Unbound command -> no preferred binding.
    ASSERT_FALSE(ssg::preferredBinding(a, "other").has_value());
}

int main() {
    RUN(commandLabelUsesAuthoredLabelsAndHumanizesTheRest);
    RUN(formatKeySequenceIsCompactAndHuman);
    RUN(preferredBindingIsDeterministic);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
