#include <ssg/PromptSurface.h>

#include <ssg/Keymap.h>

namespace ssg {

namespace {

// The typed route for the active input's new full value. Replace's query input
// (0) routes to UpdateFindQuery; its replacement input (1) to UpdateReplacement;
// Find to UpdateFindQuery; a generic TextPrompt to prompt.update_value at the
// active index.
PromptTextRoute dispatchActiveInput(ActivePrompt prompt, std::size_t activeInput,
                                    PromptEditState edited) {
    switch (prompt) {
    case ActivePrompt::Replace:
        if (activeInput == 0) {
            return {PromptTextRoute::Kind::UpdateFindQuery, edited};
        }
        return {PromptTextRoute::Kind::UpdateReplacement, edited};
    case ActivePrompt::Find:
        return {PromptTextRoute::Kind::UpdateFindQuery, edited};
    case ActivePrompt::TextPrompt:
        return {PromptTextRoute::Kind::UpdatePromptValue, edited, activeInput};
    case ActivePrompt::Palette:
    case ActivePrompt::None:
        return {};
    }
    return {};
}

}  // namespace

PromptTextRoute routePromptTextEdit(const PromptRoutingState& state,
                                    const PromptTextEdit& change) {
    switch (textRoutingForContext(focusTargetName(state.focus))) {
    case TextRouting::Insert:
        // Editor text input is routed directly in InputRouting; prompt routing owns
        // only prompt/query edits.
        return {};
    case TextRouting::PromptQuery:
        if (state.prompt == ActivePrompt::Palette) {
            // The palette query is client-owned end to end (see main.cpp's
            // PaletteView), so no edit of it is routed through this seam.
            return {};
        }
        return dispatchActiveInput(state.prompt, state.activeInput,
                                   applyPromptTextEdit(state.current, change));
    case TextRouting::Ignore:
        return {};
    }
    return {};
}

}  // namespace ssg
