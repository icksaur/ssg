// seam test — the prompt-text routing decision is an architectural rule: printable
// text under a focus and an active prompt maps to exactly one library command, a
// palette-query append, or nothing, and both the TUI app and the web host must
// resolve it through this one seam so the two clients cannot drift into separate
// input behavior.  These cases pin every branch the old app-side routeText held.

#include <ssg/FindReplace.h>
#include <ssg/PromptRouting.h>
#include <ssg/PromptSurface.h>
#include <ssg/TextInputCommands.h>

#include "test_helpers.h"

#include <any>
#include <string>

namespace {

using namespace ssg;

PromptRoutingState atEditor() {
    return {FocusTarget::Editor, ActivePrompt::None, {}};
}

PromptRoutingState atPrompt(ActivePrompt prompt, std::string value = {}) {
    return {FocusTarget::Prompt, prompt, std::move(value)};
}

TEST(editorFocusRoutesPrintableTextToInsert) {
    auto const route = PromptTextRouter{}.route(atEditor(), "x");
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Dispatch);
    ASSERT_TRUE(route.command == CommandName{"text.insert"});
    auto const* args = std::any_cast<TextInputArguments>(&route.payload);
    ASSERT_TRUE(args != nullptr);
    ASSERT_EQ(args->text, std::string{"x"});
}

TEST(paletteFocusAppendsToTheClientOwnedQueryOnly) {
    auto const route = PromptTextRouter{}.route(atPrompt(ActivePrompt::Palette), "a");
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::AppendPaletteQuery);
    ASSERT_EQ(route.appendText, std::string{"a"});
    ASSERT_TRUE(route.command.empty());
}

TEST(findPromptRoutesTheWholeNewQueryValue) {
    auto const route =
        PromptTextRouter{}.route(atPrompt(ActivePrompt::Find, "foo"), "d");
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Dispatch);
    ASSERT_TRUE(route.command == CommandName{"find.update_query"});
    auto const* args = std::any_cast<FindQueryArguments>(&route.payload);
    ASSERT_TRUE(args != nullptr);
    ASSERT_EQ(args->query, std::string{"food"});
}

TEST(replacePromptRoutesTheWholeNewReplacementValue) {
    auto const route =
        PromptTextRouter{}.route(atPrompt(ActivePrompt::Replace, "ba"), "r");
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Dispatch);
    ASSERT_TRUE(route.command == CommandName{"replace.update_replacement"});
    auto const* args = std::any_cast<FindQueryArguments>(&route.payload);
    ASSERT_TRUE(args != nullptr);
    ASSERT_EQ(args->query, std::string{"bar"});
}

TEST(textPromptRoutesTheWholeNewValueAtIndexZero) {
    auto const route =
        PromptTextRouter{}.route(atPrompt(ActivePrompt::TextPrompt, "na"), "me");
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Dispatch);
    ASSERT_TRUE(route.command == CommandName{"prompt.update_value"});
    auto const* args = std::any_cast<PromptValueArguments>(&route.payload);
    ASSERT_TRUE(args != nullptr);
    ASSERT_EQ(args->index, static_cast<std::size_t>(0));
    ASSERT_EQ(args->value, std::string{"name"});
}

TEST(promptFocusWithNoActivePromptIgnoresText) {
    auto const route = PromptTextRouter{}.route(atPrompt(ActivePrompt::None), "z");
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Ignore);
}

TEST(panelFocusIgnoresPrintableText) {
    PromptRoutingState state{FocusTarget::Panel, ActivePrompt::None, {}};
    auto const route = PromptTextRouter{}.route(state, "z");
    ASSERT_TRUE(route.kind == PromptTextRoute::Kind::Ignore);
}

}  // namespace

int main() {
    RUN(editorFocusRoutesPrintableTextToInsert);
    RUN(paletteFocusAppendsToTheClientOwnedQueryOnly);
    RUN(findPromptRoutesTheWholeNewQueryValue);
    RUN(replacePromptRoutesTheWholeNewReplacementValue);
    RUN(textPromptRoutesTheWholeNewValueAtIndexZero);
    RUN(promptFocusWithNoActivePromptIgnoresText);
    RUN(panelFocusIgnoresPrintableText);
    return 0;
}
