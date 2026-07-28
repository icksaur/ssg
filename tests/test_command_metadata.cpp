#include <ssg/CommandCatalog.h>
#include <ssg/command_metadata.h>
#include <ssg/Keymap.h>

#include "test_helpers.h"

#include <string>

TEST(commandLabelUsesAuthoredLabelsAndHumanizesTheRest) {
    // Built here rather than taken from a runtime: the rule under test is how a
    // label is DERIVED, which does not depend on which commands the editor
    // happens to offer.
    ssg::CommandCatalog catalog;
    auto declare = [&catalog](std::string id, std::string label) {
        auto spec = ssg::CommandSpecBuilder{std::move(id)}
                        .owner("test-owner")
                        .summary("a command")
                        .observes()
                        .handler([](ssg::CommandContext&) {
                            return ssg::CommandHandlerResult::success();
                        });
        if (!label.empty()) spec.label(std::move(label));
        catalog.add(std::move(spec));
    };
    declare("file.save", "Save File");
    declare("edit.undo", "Undo");
    declare("cursor.line_down", "");
    declare("tree.select_previous", "");

    auto labelOf = [&catalog](std::string_view id) {
        auto const* command = catalog.find(id);
        return command == nullptr ? std::string{} : ssg::commandLabel(*command);
    };

    // An authored label is used verbatim.
    ASSERT_EQ(labelOf("file.save"), std::string{"Save File"});
    ASSERT_EQ(labelOf("edit.undo"), std::string{"Undo"});

    // A command with no authored label humanizes from its segments, and never
    // shows the raw id.
    ASSERT_EQ(labelOf("cursor.line_down"), std::string{"Cursor Line Down"});
    ASSERT_NE(labelOf("cursor.line_down"), std::string{"cursor.line_down"});
    ASSERT_EQ(labelOf("tree.select_previous"),
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
