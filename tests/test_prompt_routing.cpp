#include <ssg/FindReplace.h>
#include <ssg/PromptSurface.h>
#include <ssg/TextInputCommands.h>

#include "test_helpers.h"

#include <string>

namespace {

using namespace ssg;

PromptRoutingState atEditor() {
    return {FocusTarget::Editor, ActivePrompt::None, {}};
}

PromptRoutingState atPrompt(ActivePrompt prompt, std::string value = {},
                            std::size_t activeInput = 0) {
    return {FocusTarget::Prompt, prompt, std::move(value), activeInput};
}

TEST(editorFocusLeavesPrintableTextToInputRouting) {
    auto const route = routePromptTextEdit(
        atEditor(), {PromptTextEdit::Kind::Append, "x"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Ignore);
}

TEST(paletteFocusAppendsToTheClientOwnedQueryOnly) {
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Palette),
        {PromptTextEdit::Kind::Append, "a"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::AppendPaletteQuery);
    ASSERT_EQ(route.appendText, std::string{"a"});
    ASSERT_TRUE(route.query.empty());
}

TEST(findPromptRoutesTheWholeNewQueryValue) {
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Find, "foo"),
        {PromptTextEdit::Kind::Append, "d"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateFindQuery);
    ASSERT_EQ(route.query, std::string{"food"});
}

TEST(replacePromptRoutesTheWholeNewReplacementValue) {
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Replace, "ba", 1),
        {PromptTextEdit::Kind::Append, "r"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateReplacement);
    ASSERT_EQ(route.query, std::string{"bar"});
}

TEST(replaceQueryInputRoutesToFindUpdateQuery) {
    // Replace's query input (active index 0) edits the SAME query as Find, so it
    // must route to UpdateFindQuery, not UpdateReplacement.
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Replace, "fo", 0),
        {PromptTextEdit::Kind::Append, "o"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateFindQuery);
    ASSERT_EQ(route.query, std::string{"foo"});
}

TEST(promptEditDeletesOneGraphemeBackFromTheActiveInput) {
    PromptTextEdit back{PromptTextEdit::Kind::DeleteGraphemeBack, {}};
    auto const route =
        routePromptTextEdit(atPrompt(ActivePrompt::Find, "café", 0), back);
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateFindQuery);
    // The multi-byte 'é' is one grapheme, so one backspace removes it whole.
    ASSERT_EQ(route.query, std::string{"caf"});
}

TEST(promptEditDeletesOneWordBackFromTheActiveReplacement) {
    PromptTextEdit back{PromptTextEdit::Kind::DeleteWordBack, {}};
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Replace, "one two ", 1), back);
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateReplacement);
    ASSERT_EQ(route.query, std::string{"one "});
}

TEST(paletteDeletionIsNotRoutedThroughTheSeam) {
    PromptTextEdit back{PromptTextEdit::Kind::DeleteGraphemeBack, {}};
    auto const route =
        routePromptTextEdit(atPrompt(ActivePrompt::Palette, "ab"), back);
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Ignore);
}

TEST(textPromptRoutesTheWholeNewValueAtIndexZero) {
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::TextPrompt, "na"),
        {PromptTextEdit::Kind::Append, "me"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdatePromptValue);
    ASSERT_EQ(route.promptValue.index, static_cast<std::size_t>(0));
    ASSERT_EQ(route.promptValue.value, std::string{"name"});
}

TEST(genericTextPromptDeletionRoutesToPromptUpdateValueNotAHardcodedField) {
    PromptTextEdit back{PromptTextEdit::Kind::DeleteGraphemeBack, {}};
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::TextPrompt, "name", 0), back);
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdatePromptValue);
    ASSERT_EQ(route.promptValue.index, static_cast<std::size_t>(0));
    ASSERT_EQ(route.promptValue.value, std::string{"nam"});
}

TEST(promptFocusWithNoActivePromptIgnoresText) {
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::None),
        {PromptTextEdit::Kind::Append, "z"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Ignore);
}

TEST(panelFocusIgnoresPrintableText) {
    PromptRoutingState state{FocusTarget::Panel, ActivePrompt::None, {}};
    auto const route =
        routePromptTextEdit(state, {PromptTextEdit::Kind::Append, "z"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Ignore);
}

}  // namespace

SSG_TEST_SUITE(test_prompt_routing) {
    RUN(editorFocusLeavesPrintableTextToInputRouting);
    RUN(paletteFocusAppendsToTheClientOwnedQueryOnly);
    RUN(findPromptRoutesTheWholeNewQueryValue);
    RUN(replacePromptRoutesTheWholeNewReplacementValue);
    RUN(replaceQueryInputRoutesToFindUpdateQuery);
    RUN(promptEditDeletesOneGraphemeBackFromTheActiveInput);
    RUN(promptEditDeletesOneWordBackFromTheActiveReplacement);
    RUN(paletteDeletionIsNotRoutedThroughTheSeam);
    RUN(textPromptRoutesTheWholeNewValueAtIndexZero);
    RUN(genericTextPromptDeletionRoutesToPromptUpdateValueNotAHardcodedField);
    RUN(promptFocusWithNoActivePromptIgnoresText);
    RUN(panelFocusIgnoresPrintableText);
    return 0;
}
