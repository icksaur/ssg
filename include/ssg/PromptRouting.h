#pragma once

#include <ssg/CommandHandle.h>
#include <ssg/focus.h>

#include <any>
#include <cstdint>
#include <string>

namespace ssg {

// Which prompt currently owns printable text.  Exactly one is active at a time;
// modeling it as one enum rather than several open/closed bools makes "two
// prompts open at once" unrepresentable, which is the state the old app-side
// routeText priority ladder existed to disambiguate.
enum class ActivePrompt : std::uint8_t {
    None,
    Palette,
    Find,
    Replace,
    TextPrompt,
};

struct PromptRoutingState {
    FocusTarget focus = FocusTarget::Editor;
    ActivePrompt prompt = ActivePrompt::None;
    // The current value of the ACTIVE input for Find, Replace, and TextPrompt.
    // The seam appends or deletes and returns the whole value, matching how
    // find.update_query / replace.update_replacement / prompt.update_value each
    // take the full value.  Unused for Palette (the client-owned derived view)
    // and None.
    std::string currentValue;
    // Which input of the active prompt owns the keyboard, mirroring the library's
    // PromptSurface::activeInput. Replace's query is 0 and its replacement is 1;
    // single-input prompts are 0. It selects both the command (query vs
    // replacement) and the update_value index.
    std::size_t activeInput = 0;
};

// An edit to the active prompt input: append printable text, or delete backward
// by one grapheme cluster or one word. The seam owns append AND grapheme/word-
// aware deletion so every host edits the same way; a host never re-implements
// backspace semantics.
struct PromptTextEdit {
    enum class Kind : std::uint8_t {
        Append,
        DeleteGraphemeBack,
        DeleteWordBack,
    } kind = Kind::Append;
    // Valid when kind == Append.
    std::string text;
};

struct PromptTextRoute {
    enum class Kind : std::uint8_t {
        Dispatch,
        AppendPaletteQuery,
        Ignore,
    } kind = Kind::Ignore;

    // Valid when kind == Dispatch: a library command and its fully-built
    // argument payload, ready to dispatch through the runtime.
    CommandName command;
    std::any payload;

    // Valid when kind == AppendPaletteQuery: the text the client appends to its
    // own palette query -- the one client-owned derived view under the
    // prompt-fulfilment boundary.
    std::string appendText;
};

// The single decision that maps a text edit, a focus, and the active prompt to a
// library command, a palette-query append, or nothing.  Both the TUI app and the
// web host call this, so the routing AND the delete semantics exist once, not
// once per client.
class PromptTextRouter {
public:
    // Append printable text to the active input. Preserved for callers that only
    // append; equivalent to edit with a DeleteGraphemeBack-free Append.
    [[nodiscard]] PromptTextRoute route(PromptRoutingState const& state,
                                        std::string const& text) const;
    // Append or delete backward on the active input, returning the command that
    // carries the input's NEW full value. Palette deletion is client-owned and
    // returns Ignore; palette append returns AppendPaletteQuery.
    [[nodiscard]] PromptTextRoute edit(PromptRoutingState const& state,
                                       PromptTextEdit const& change) const;
};

}  // namespace ssg
