#include <ssg/PromptSurface.h>

#include <ssg/FindReplace.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/Keymap.h>
#include <ssg/TextInputCommands.h>

namespace ssg {

std::string applyPromptTextEdit(std::string_view value,
                                const PromptTextEdit& change) {
    switch (change.kind) {
    case PromptTextEdit::Kind::Append:
        return std::string(value) + change.text;
    case PromptTextEdit::Kind::DeleteGraphemeBack: {
        if (value.empty()) return std::string(value);
        // The last cluster's start byte is where the trailing grapheme (with any
        // absorbed combining marks) begins, so truncating there deletes exactly
        // one user-perceived character.
        CellRun run = computeCellRun(value);
        if (run.spans.empty()) return std::string(value);
        return std::string(value.substr(0, run.spans.back().byteOffset));
    }
    case PromptTextEdit::Kind::DeleteWordBack: {
        std::size_t end = value.size();
        while (end > 0 &&
               !isWordByte(static_cast<unsigned char>(value[end - 1]))) {
            --end;
        }
        while (end > 0 &&
               isWordByte(static_cast<unsigned char>(value[end - 1]))) {
            --end;
        }
        return std::string(value.substr(0, end));
    }
    }
    return std::string(value);
}

namespace {

// The command carrying the active input's new full value. Replace's query input
// (0) routes to find.update_query; its replacement input (1) to
// replace.update_replacement; Find to find.update_query; a generic TextPrompt to
// prompt.update_value at the active index.
PromptTextRoute dispatchActiveInput(ActivePrompt prompt, std::size_t activeInput,
                                    std::string value) {
    switch (prompt) {
    case ActivePrompt::Replace:
        if (activeInput == 0) {
            return {PromptTextRoute::Kind::Dispatch,
                    CommandName{"find.update_query"},
                    FindQueryArguments{std::move(value)}, {}};
        }
        return {PromptTextRoute::Kind::Dispatch,
                CommandName{"replace.update_replacement"},
                FindQueryArguments{std::move(value)}, {}};
    case ActivePrompt::Find:
        return {PromptTextRoute::Kind::Dispatch,
                CommandName{"find.update_query"},
                FindQueryArguments{std::move(value)}, {}};
    case ActivePrompt::TextPrompt: {
        PromptTextRoute route;
        route.kind = PromptTextRoute::Kind::UpdatePromptValue;
        route.promptValue = PromptValueArguments{activeInput, std::move(value)};
        return route;
    }
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
            // The palette query is the one client-owned derived view; the client
            // appends its own text and pops its own graphemes. Deletion is not
            // routed here.
            if (change.kind != PromptTextEdit::Kind::Append) return {};
            return {PromptTextRoute::Kind::AppendPaletteQuery, {}, {},
                    change.text};
        }
        return dispatchActiveInput(state.prompt, state.activeInput,
                                   applyPromptTextEdit(state.currentValue,
                                                       change));
    case TextRouting::Ignore:
        return {};
    }
    return {};
}

}  // namespace ssg
