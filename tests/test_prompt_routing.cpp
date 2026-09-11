#include <ssg/FindReplace.h>
#include <ssg/PromptSurface.h>
#include <ssg/TextInputCommands.h>

#include "test_helpers.h"

#include <string>

namespace {

using namespace ssg;

PromptRoutingState atEditor() {
    return {FocusTarget::Editor, ActivePrompt::None, {}, 0};
}

PromptRoutingState atPrompt(ActivePrompt prompt, std::string value = {},
                            std::size_t activeInput = 0,
                            std::size_t cursor = std::string::npos) {
    if (cursor == std::string::npos) cursor = value.size();
    return {FocusTarget::Prompt, prompt, {std::move(value), cursor}, activeInput};
}

TEST(editorFocusLeavesPrintableTextToInputRouting) {
    auto const route = routePromptTextEdit(
        atEditor(), {PromptTextEdit::Kind::Insert, "x"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Ignore);
}

TEST(paletteEditsAreNeverRoutedThroughTheSharedSeam) {
    // The palette query is client-owned (see main.cpp's PaletteView), so every
    // edit kind must fall through to Ignore here, not just deletion.
    for (auto const kind : {
             PromptTextEdit::Kind::Insert,
             PromptTextEdit::Kind::MoveLeft,
             PromptTextEdit::Kind::MoveRight,
             PromptTextEdit::Kind::MoveToStart,
             PromptTextEdit::Kind::MoveToEnd,
             PromptTextEdit::Kind::DeleteBackward,
             PromptTextEdit::Kind::DeleteForward,
             PromptTextEdit::Kind::DeleteWordBackward,
             PromptTextEdit::Kind::DeleteWordForward,
         }) {
        auto const route = routePromptTextEdit(
            atPrompt(ActivePrompt::Palette, "ab"), {kind, "a"});
        ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Ignore);
    }
}

TEST(findPromptRoutesTheWholeNewQueryValue) {
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Find, "foo"),
        {PromptTextEdit::Kind::Insert, "d"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateFindQuery);
    ASSERT_EQ(route.edited.text(), std::string{"food"});
    ASSERT_EQ(route.edited.cursor(), static_cast<std::size_t>(4));
}

TEST(replacePromptRoutesTheWholeNewReplacementValue) {
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Replace, "ba", 1, 1),
        {PromptTextEdit::Kind::Insert, "r"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateReplacement);
    ASSERT_EQ(route.edited.text(), std::string{"bra"});
    ASSERT_EQ(route.edited.cursor(), static_cast<std::size_t>(2));
}

TEST(replaceQueryInputRoutesToFindUpdateQuery) {
    // Replace's query input (active index 0) edits the SAME query as Find, so it
    // must route to UpdateFindQuery, not UpdateReplacement.
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Replace, "fo", 0),
        {PromptTextEdit::Kind::Insert, "o"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateFindQuery);
    ASSERT_EQ(route.edited.text(), std::string{"foo"});
}

TEST(promptEditMovesTheCursorWithoutChangingTheText) {
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Find, "foo", 0, 3),
        {PromptTextEdit::Kind::MoveLeft, {}});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateFindQuery);
    ASSERT_EQ(route.edited.text(), std::string{"foo"});
    ASSERT_EQ(route.edited.cursor(), static_cast<std::size_t>(2));
}

TEST(promptEditDeletesOneGraphemeBackFromTheActiveInput) {
    PromptTextEdit back{PromptTextEdit::Kind::DeleteBackward, {}};
    auto const route =
        routePromptTextEdit(atPrompt(ActivePrompt::Find, "café", 0), back);
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateFindQuery);
    // The multi-byte 'é' is one grapheme, so one backspace removes it whole.
    ASSERT_EQ(route.edited.text(), std::string{"caf"});
}

TEST(promptEditDeletesOneGraphemeForwardFromTheActiveInput) {
    PromptTextEdit forward{PromptTextEdit::Kind::DeleteForward, {}};
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Find, "café", 0, 0), forward);
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateFindQuery);
    ASSERT_EQ(route.edited.text(), std::string{"afé"});
    ASSERT_EQ(route.edited.cursor(), static_cast<std::size_t>(0));
}

TEST(promptEditDeletesOneWordBackFromTheActiveReplacement) {
    PromptTextEdit back{PromptTextEdit::Kind::DeleteWordBackward, {}};
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Replace, "one two ", 1), back);
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateReplacement);
    ASSERT_EQ(route.edited.text(), std::string{"one "});
}

TEST(promptEditDeletesOneWordForwardFromTheActiveReplacement) {
    PromptTextEdit forward{PromptTextEdit::Kind::DeleteWordForward, {}};
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::Replace, " two three", 1, 0), forward);
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdateReplacement);
    ASSERT_EQ(route.edited.text(), std::string{" three"});
}

TEST(textPromptRoutesTheWholeNewValueAtIndexZero) {
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::TextPrompt, "na"),
        {PromptTextEdit::Kind::Insert, "me"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdatePromptValue);
    ASSERT_EQ(route.index, static_cast<std::size_t>(0));
    ASSERT_EQ(route.edited.text(), std::string{"name"});
}

TEST(genericTextPromptDeletionRoutesToPromptUpdateValueNotAHardcodedField) {
    PromptTextEdit back{PromptTextEdit::Kind::DeleteBackward, {}};
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::TextPrompt, "name", 0), back);
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::UpdatePromptValue);
    ASSERT_EQ(route.index, static_cast<std::size_t>(0));
    ASSERT_EQ(route.edited.text(), std::string{"nam"});
}

TEST(promptFocusWithNoActivePromptIgnoresText) {
    auto const route = routePromptTextEdit(
        atPrompt(ActivePrompt::None),
        {PromptTextEdit::Kind::Insert, "z"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Ignore);
}

TEST(panelFocusIgnoresPrintableText) {
    PromptRoutingState state{FocusTarget::Panel, ActivePrompt::None, {}, 0};
    auto const route =
        routePromptTextEdit(state, {PromptTextEdit::Kind::Insert, "z"});
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Ignore);
}

}  // namespace

SSG_TEST_SUITE(test_prompt_routing) {
    RUN(editorFocusLeavesPrintableTextToInputRouting);
    RUN(paletteEditsAreNeverRoutedThroughTheSharedSeam);
    RUN(findPromptRoutesTheWholeNewQueryValue);
    RUN(replacePromptRoutesTheWholeNewReplacementValue);
    RUN(replaceQueryInputRoutesToFindUpdateQuery);
    RUN(promptEditMovesTheCursorWithoutChangingTheText);
    RUN(promptEditDeletesOneGraphemeBackFromTheActiveInput);
    RUN(promptEditDeletesOneGraphemeForwardFromTheActiveInput);
    RUN(promptEditDeletesOneWordBackFromTheActiveReplacement);
    RUN(promptEditDeletesOneWordForwardFromTheActiveReplacement);
    RUN(textPromptRoutesTheWholeNewValueAtIndexZero);
    RUN(genericTextPromptDeletionRoutesToPromptUpdateValueNotAHardcodedField);
    RUN(promptFocusWithNoActivePromptIgnoresText);
    RUN(panelFocusIgnoresPrintableText);
    return 0;
}
