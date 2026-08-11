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
    // The current query/value for Find, Replace, and TextPrompt.  The seam
    // appends the new text and returns the whole value, matching how
    // find.update_query / replace.update_replacement / prompt.update_value each
    // take the full value.  Unused for Palette (the client-owned derived view)
    // and None.
    std::string currentValue;
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

// The single decision that maps printable text, a focus, and the active prompt
// to a library command, a palette-query append, or nothing.  Both the TUI app
// and the web host call this, so the routing exists once, not once per client.
class PromptTextRouter {
public:
    [[nodiscard]] PromptTextRoute route(PromptRoutingState const& state,
                                        std::string const& text) const;
};

}  // namespace ssg
