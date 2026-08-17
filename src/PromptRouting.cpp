#include <ssg/PromptRouting.h>

#include <ssg/FindReplace.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/Keymap.h>
#include <ssg/PromptSurface.h>
#include <ssg/TextInputCommands.h>
#include <ssg/WordClassification.h>

namespace ssg {

namespace {

std::string applyEdit(std::string_view value, PromptTextEdit const& change) {
    switch (change.kind) {
    case PromptTextEdit::Kind::Append:
        return std::string(value) + change.text;
    case PromptTextEdit::Kind::DeleteGraphemeBack: {
        if (value.empty()) return std::string(value);
        // The last cluster's start byte is where the trailing grapheme (with any
        // absorbed combining marks) begins, so truncating there deletes exactly
        // one user-perceived character.
        CellRun run = GraphemeLayout{}.computeRun(value);
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
    case ActivePrompt::TextPrompt:
        return {PromptTextRoute::Kind::Dispatch,
                CommandName{"prompt.update_value"},
                PromptValueArguments{activeInput, std::move(value)}, {}};
    case ActivePrompt::Palette:
    case ActivePrompt::None:
        return {};
    }
    return {};
}

}  // namespace

PromptTextRoute PromptTextRouter::route(PromptRoutingState const& state,
                                        std::string const& text) const {
    return edit(state, PromptTextEdit{PromptTextEdit::Kind::Append, text});
}

PromptTextRoute PromptTextRouter::edit(PromptRoutingState const& state,
                                       PromptTextEdit const& change) const {
    switch (SemanticInputRouter{}.textRouting(focusTargetName(state.focus))) {
    case TextRouting::Insert:
        // A DeleteGraphemeBack/DeleteWordBack in the editor is not this seam's
        // job; only an append inserts text. The editor's own delete commands
        // handle backspace when the editor holds focus.
        if (change.kind != PromptTextEdit::Kind::Append) return {};
        return {PromptTextRoute::Kind::Dispatch, CommandName{"text.insert"},
                TextInputArguments{change.text}, {}};
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
                                   applyEdit(state.currentValue, change));
    case TextRouting::Ignore:
        return {};
    }
    return {};
}

}  // namespace ssg
