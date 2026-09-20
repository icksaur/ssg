# Configuring ssg

SSG has no runtime scripting language or config file. Configuration is compiled
in: edit `src/UserConfig.cpp`, implement `applyUserConfig(Editor&)`, and
rebuild.

The default implementation is a no-op. If you leave it alone, SSG starts with
its built-in theme, style, and keymap.

## Entry points

`applyUserConfig(Editor&)` is expected to use the same configuration operations
the runtime already uses internally:

- `applyThemeSet(Editor&, ThemeSetArguments)`
- `applyStyleDefine(Editor&, StyleDefineArguments)`
- `applyKeymapBind(Editor&, KeymapBindArguments)`
- `applyKeymapUnbind(Editor&, KeymapUnbindArguments)`

Each returns an `OperationResult`. Handle a rejected result explicitly; the
default `main()` path reports any exception thrown from `applyUserConfig`.

## Example

```cpp
#include <stdexcept>

#include <ssg/UserConfig.h>

namespace ssg {

void applyUserConfig(Editor& editor) {
    if (auto result = applyThemeSet(
            editor,
            ThemeSetArguments{{{"keyword", "#ab47bc"},
                               {"selection", "#2a4e2e"}}});
        !result.accepted) {
        throw std::runtime_error{result.message};
    }

    if (auto result = applyStyleDefine(
            editor,
            StyleDefineArguments{{{"tab_separator", " | "},
                                  {"tree_collapsed", "> "}}});
        !result.accepted) {
        throw std::runtime_error{result.message};
    }

    if (auto result =
            applyKeymapBind(editor, {"Mod+KeyH", "help.open", "*"});
        !result.accepted) {
        throw std::runtime_error{result.message};
    }

    if (auto result = applyKeymapUnbind(editor, {"Mod+KeyS", "*"});
        !result.accepted) {
        throw std::runtime_error{result.message};
    }
}

}  // namespace ssg
```

## Theme colors

`applyThemeSet` maps role and scope names directly to `"#rrggbb"` strings. The
table may be partial: omitted names keep their existing value. An unknown name
or malformed color rejects the whole call.

The supported names are the semantic roles and syntax scopes defined in
`include/ssg/Theme.h`. The built-in defaults live in `src/DefaultTheme.cpp`.

## Key bindings

`KeymapBindArguments` takes:

- `sequence` — one stroke such as `"Mod+KeyS"` or `"Mod+Shift+KeyM"`
- `command` — the command id to run
- `context` — `"*"`, `"editor"`, `"panel"`, or `"prompt"`

`Mod` means Ctrl or Alt. Multi-stroke bindings are rejected. `applyKeymapBind`
replaces any existing binding for the same `(sequence, context)` pair, and
`applyKeymapUnbind` removes one if present.

## Chrome glyphs and dimensions

`applyStyleDefine` updates the glyphs and dimension values from
`include/ssg/Style.h`. The table may be partial. Unknown keys, invalid UTF-8,
control characters, wrong-width glyphs, or negative dimensions reject the whole
call.

## Terminal capabilities

At startup ssg asks your terminal what it supports -- synchronized output,
the keyboard protocol, clipboard access -- and adapts. Nothing waits for the
answers, so a terminal that stays silent simply gets the conservative
rendering rather than a slow start.

Terminals sometimes claim a capability they render badly, or omit one they
actually have. Every answer can be overridden, and an override always beats
what the terminal reports:

- `SSG_TERM_SYNCHRONIZED_OUTPUT` -- tear-free full-frame redraw.
- `SSG_TERM_KEYBOARD_PROTOCOL` -- disambiguated key reporting.
- `SSG_TERM_CLIPBOARD_WRITE` -- copying to your system clipboard.

`Mod+V` reads the desktop clipboard with `wl-paste` on Wayland or `xclip` on
X11 when the corresponding helper is available. These are optional runtime
helpers, not terminal capability overrides.

Each takes `on`/`off` (`1`/`0`, `yes`/`no`, `true`/`false` also work). A
value that isn't one of those is ignored, and the terminal's own answer
stands. For example, to turn one off for a session:

```
SSG_TERM_SYNCHRONIZED_OUTPUT=off ssg
```

Colour depth is resolved the same way and forced with `SSG_COLOR_DEPTH`
(`truecolor`/`24bit`, `256`/`256color`/`indexed256`, or `16`/`ansi16`).

Two notes worth knowing. Capabilities belong to the *connection*, not the
machine -- the same computer answers differently over ssh, inside tmux, or
from a different terminal emulator -- so nothing is cached between runs. And
a multiplexer answers on its own behalf: under tmux or screen you get what
the multiplexer supports, which may be less than the terminal behind it.
That is correct, not a bug: the multiplexer is the terminal ssg is talking
to.
