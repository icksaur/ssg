#include <ssg/PromptRouting.h>

#include <ssg/FindReplace.h>
#include <ssg/Keymap.h>
#include <ssg/PromptSurface.h>
#include <ssg/TextInputCommands.h>

namespace ssg {

PromptTextRoute PromptTextRouter::route(PromptRoutingState const& state,
                                        std::string const& text) const {
    switch (SemanticInputRouter{}.textRouting(focusTargetName(state.focus))) {
    case TextRouting::Insert:
        return {PromptTextRoute::Kind::Dispatch, CommandName{"text.insert"},
                TextInputArguments{text}, {}};
    case TextRouting::PromptQuery:
        switch (state.prompt) {
        case ActivePrompt::Palette:
            return {PromptTextRoute::Kind::AppendPaletteQuery, {}, {}, text};
        case ActivePrompt::Replace:
            return {PromptTextRoute::Kind::Dispatch,
                    CommandName{"replace.update_replacement"},
                    FindQueryArguments{state.currentValue + text}, {}};
        case ActivePrompt::Find:
            return {PromptTextRoute::Kind::Dispatch,
                    CommandName{"find.update_query"},
                    FindQueryArguments{state.currentValue + text}, {}};
        case ActivePrompt::TextPrompt:
            return {PromptTextRoute::Kind::Dispatch,
                    CommandName{"prompt.update_value"},
                    PromptValueArguments{0, state.currentValue + text}, {}};
        case ActivePrompt::None:
            return {};
        }
        return {};
    case TextRouting::Ignore:
        return {};
    }
    return {};
}

}  // namespace ssg
