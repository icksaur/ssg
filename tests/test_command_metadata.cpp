#include <ssg/command_metadata.h>
#include <ssg/input.h>

#include "test_helpers.h"

#include <string>

TEST(command_label_uses_authored_labels_and_humanizes_the_rest) {
    // Authored labels for common commands.
    ASSERT_EQ(ssg::command_label("file.save"), std::string{"Save File"});
    ASSERT_EQ(ssg::command_label("edit.undo"), std::string{"Undo"});
    ASSERT_EQ(ssg::command_label("palette.open"),
              std::string{"Command Palette"});

    // Uncurated ids humanize from their segments, never showing the raw id.
    ASSERT_EQ(ssg::command_label("cursor.line_down"),
              std::string{"Cursor Line Down"});
    ASSERT_NE(ssg::command_label("cursor.line_down"),
              std::string{"cursor.line_down"});
    ASSERT_EQ(ssg::command_label("tree.select_previous"),
              std::string{"Tree Select Previous"});
}

TEST(format_key_sequence_is_compact_and_human) {
    ASSERT_EQ(ssg::format_key_sequence(*ssg::parse_key_sequence({"Escape", "KeyS"})),
              std::string{"Esc S"});
    ASSERT_EQ(ssg::format_key_sequence(*ssg::parse_key_sequence({"ArrowDown"})),
              std::string{"Down"});
    ASSERT_EQ(ssg::format_key_sequence(
                  *ssg::parse_key_sequence({"Escape", "Shift+KeyZ"})),
              std::string{"Esc Shift+Z"});
    ASSERT_EQ(ssg::format_key_sequence(
                  *ssg::parse_key_sequence({"Escape", "BracketRight"})),
              std::string{"Esc ]"});
    ASSERT_TRUE(ssg::format_key_sequence({}).empty());
}

TEST(preferred_binding_is_deterministic) {
    const auto short_seq = *ssg::parse_key_sequence({"Escape", "KeyS"});
    const auto long_seq = *ssg::parse_key_sequence({"Escape", "KeyF", "KeyT"});
    // Two bindings for one command: the shorter wins regardless of order.
    ssg::KeymapViewState a{"m", {{long_seq, "cmd", "*"}, {short_seq, "cmd", "*"}}};
    ssg::KeymapViewState b{"m", {{short_seq, "cmd", "*"}, {long_seq, "cmd", "*"}}};
    auto a_pref = ssg::preferred_binding(a, "cmd");
    auto b_pref = ssg::preferred_binding(b, "cmd");
    ASSERT_TRUE(a_pref.has_value());
    ASSERT_TRUE(b_pref.has_value());
    ASSERT_EQ(*a_pref, short_seq);
    ASSERT_EQ(*b_pref, short_seq);

    // Equal length: the lexicographically least display form wins.
    const auto esc_a = *ssg::parse_key_sequence({"Escape", "KeyA"});
    const auto esc_b = *ssg::parse_key_sequence({"Escape", "KeyB"});
    ssg::KeymapViewState c{"m", {{esc_b, "cmd", "*"}, {esc_a, "cmd", "*"}}};
    auto c_pref = ssg::preferred_binding(c, "cmd");
    ASSERT_TRUE(c_pref.has_value());
    ASSERT_EQ(*c_pref, esc_a);

    // Unbound command -> no preferred binding.
    ASSERT_FALSE(ssg::preferred_binding(a, "other").has_value());
}

int main() {
    RUN(command_label_uses_authored_labels_and_humanizes_the_rest);
    RUN(format_key_sequence_is_compact_and_human);
    RUN(preferred_binding_is_deterministic);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
