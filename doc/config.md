# Configuring ssg

ssg loads one Lua script at startup, `init.lua`, and runs whatever
`ssg.command(...)` calls it contains. There is no other configuration file
format today.

## Where `init.lua` lives

- Linux: `~/.config/ssg/init.lua` (or `$XDG_CONFIG_HOME/ssg/init.lua` if you
  have `XDG_CONFIG_HOME` set).
- Windows: `%APPDATA%\ssg\init.lua`.

If the file doesn't exist, ssg starts normally with its built-in defaults --
this is not an error, and nothing is printed.

If the file exists but has a Lua syntax error, throws a runtime error, or
runs too long, ssg prints one line to stderr describing the problem and
still starts normally with whatever configuration ran successfully before
the error (usually none, since a syntax error fails the whole script before
anything executes).

`init.lua` is watched while ssg runs: editing and saving it re-runs the
whole script a moment later, without restarting ssg. Every re-run starts
from ssg's built-in defaults (not from whatever the previous run left
in place), so `init.lua`'s current content is always the *whole*
configuration -- delete a line and save, and that change reverts on the
next reload. A broken edit behaves the same as a broken file at startup:
one stderr line, and ssg keeps running with whatever the last *good* run
configured.

## What you can do today

`init.lua` can change palette colors and rebind keys.

### Colors

```lua
ssg.command("theme.define", {
    red = "#e06c75",
    green = "#98c379",
    blue = "#61afef",
})
```

`theme.define`'s table takes the 16 classic ANSI terminal color names, each
mapped to a `"#rrggbb"` hex string:

```
black   red   green   yellow   blue   magenta   cyan   white
brightBlack   brightRed   brightGreen   brightYellow
brightBlue    brightMagenta    brightCyan    brightWhite
```

You don't need to specify all 16 -- any name you omit keeps its current
color from ssg's built-in theme. This only changes the 16 raw colors; it
does not change which UI element (foreground text, selection highlight,
diff-added lines, etc.) uses which color slot -- that mapping is fixed by
ssg's built-in theme today.

### Background wash intensity

The diff and selection backgrounds are taken straight from the palette, which
can be stronger than you want underneath text. `theme.background` scales them
without touching the palette itself, so foreground text, tabs and tree rows keep
the full-strength color:

```lua
ssg.command("theme.background", {
    brightness = "0.85",              -- every wash
    saturation = "0.70",
    selection_brightness = "1.10",    -- override one
    diff_added_saturation = "0.55",
})
```

Values are numeric strings. `brightness` and `saturation` apply to all four
targets; `<target>_brightness` and `<target>_saturation` override one, where
target is `diff_added`, `diff_removed`, `diff_modified` (the modified-row
wash), or `selection`. Anything you omit stays at 1.0, which means unchanged.
Multipliers may exceed 1.0 to strengthen a wash; they are clamped, so a large
value saturates rather than wrapping. Hue is never altered.

### Key bindings

```lua
ssg.command("keymap.bind", {
    sequence = "Alt+KeyG",
    command = "find.open",
})
ssg.command("keymap.unbind", {
    sequence = "Alt+KeyS",
})
```

Frequent actions bind to single `Alt+<key>` chords, which a terminal transmits
as the same bytes as pressing Escape then the key -- so `Alt+S` saves, `Alt+P`
opens the file finder, `Alt+Shift+P` the command palette.  Escape is a plain key
that cancels a prompt or closes find in one press.  On macOS the terminal must
be set to treat Option as Meta (iTerm2: "Use Option as Meta"; Terminal.app: "Use
Option as Meta key"), or `Option+<letter>` inserts a composed character instead.

On a terminal that supports the keyboard protocol (kitty, foot, WezTerm,
ghostty, recent xterm.js and others), ssg enables it automatically and decodes
these chords from the terminal's exact modifier report instead of the
Escape-prefix bytes.  This makes `Alt+Shift+<letter>` and `Ctrl+<letter>`
bindings unambiguous and immune to Caps Lock -- with the legacy encoding, Caps
Lock inverts letter case and could swap `Alt+P` and `Alt+Shift+P`.  It is enabled
only when the terminal answers the capability query; `SSG_TERM_KEYBOARD_PROTOCOL=off`
forces the legacy path if a terminal advertises it but behaves badly.

- `sequence` is a single key stroke, e.g. `"Alt+KeyS"` or `"Ctrl+Shift+KeyM"`.
  It is an optional `Ctrl+`/`Alt+`/`Meta+`/`Shift+` prefix followed by one key
  name: `KeyA`-`KeyZ`, `Digit0`-`Digit9`, `F1`-`F24`, or a named key (`Escape`,
  `Enter`, `Tab`, `Space`, `Backspace`, `Delete`, the arrow keys,
  `Home`/`End`/`PageUp`/`PageDown`, and punctuation names like
  `BracketLeft`/`Comma`/`Slash`).  Multi-stroke sequences are not supported:
  a value naming more than one stroke is rejected.
- `command` is the command id to run (the same ids used throughout ssg,
  e.g. `file.save`, `edit.undo`, `tab.next`).
- `context` is optional and defaults to `"*"` (every focus target); it can
  instead be `"editor"`, `"panel"`, or `"prompt"` to bind only while that
  part of the UI has focus.
- `keymap.bind` replaces any existing binding for the same
  `(sequence, context)` pair rather than adding a duplicate. It's rejected
  -- leaving the keymap unchanged -- if the sequence is unparseable or names
  more than one stroke, the context is unknown, `command` is empty, or the
  result would be an invalid keymap (e.g. removing the last `Alt+Shift+KeyT`
  -> `settings.open` binding, ssg's built-in escape hatch to the Settings
  screen).
- `keymap.unbind` removes any binding matching `(sequence, context)`;
  unbinding something that isn't bound is not an error. It's rejected on
  the same "would remove the last `settings.open` binding" ground as
  `keymap.bind`.
- Bound commands show up in the command palette (`Alt+Shift+KeyP`) with
  their current key sequence next to them.
- Binding a command that needs more than a keystroke to do anything
  useful (e.g. `settings.set`, `cursor.set_position`, `text.insert`) is
  not rejected today, but pressing that key silently does nothing --
  those commands aren't reachable this way yet.

### Chrome glyphs and dimensions

`style.define` changes the glyphs ssg draws its own furniture with -- the
scrollbar track and thumb, the tree's expand/collapse arrows, the tab dirty
marker, and so on -- and the sizes of the header, footer, tab bar, scrollbar
gutter, and panel:

```lua
ssg.command("style.define", {
  scrollbar_track = ":",
  scrollbar_body = "#",
  tree_expanded = "v ",
  tree_collapsed = "> ",
})
```

The table is partial: any key you omit keeps its current value. Glyph values
are the literal string to draw; dimension keys start with `dim_` and take a
whole number (e.g. `dim_header_height = 1`). An unknown key, or a non-numeric
or negative dimension, rejects the whole call and changes nothing. The full key
list matches the style fields in `include/ssg/Style.h`.

A glyph has to fit the slot it draws in, so it must be **exactly as wide as the
one it replaces** -- one column for `scrollbar_track`, two for `tree_expanded`,
and so on. A double-width character (most CJK, and many emoji) counts as two.
A glyph of the wrong width is rejected and named, rather than silently shifting
the rest of the row sideways.

Glyphs also cannot contain **control characters** -- including escape -- or
invalid UTF-8. A stray escape sequence in a glyph does not draw: it changes how
your terminal interprets everything after it, which usually looks like the
whole screen turning into line-drawing characters. If you paste a glyph from
somewhere and it is rejected for this, the string picked up an invisible
character along the way.

### Your own commands

`ssg.register_command` defines a command in your own words and gives it a
name. It becomes a real ssg command: it shows up in the command palette, you
can bind it to a key with `keymap.bind`, and it runs the same way every
built-in does.

```lua
ssg.register_command("my.hotpink", function()
    ssg.command("theme.define", { magenta = "#ff00ff" })
    ssg.command("theme.background", { brightness = "0.6" })
end)

ssg.command("keymap.bind", {
    sequence = "Alt+KeyU",
    command = "my.hotpink",
})
```

Pick a name with a prefix of your own (`my.`, or your initials) so it can't
collide with a built-in. If it does collide, or if you register the same name
twice in one file, the whole script is rejected and your previous commands keep
working.

Your commands live exactly as long as the lines that define them. Every reload
replaces the whole set: delete a `register_command` line and save, and that
command stops existing. A key still bound to it does nothing. A reload that
fails leaves your previous commands in place and working.

**Your function may only call the commands listed on this page.** These are the
same ones `init.lua` can call directly -- `theme.define`, `theme.background`,
`style.define`, `keymap.bind` and `keymap.unbind`. Anything else, including
things like `file.save`, is refused. That list is deliberately small and will
grow deliberately.

Two things about `ssg.command` inside a registered function are worth knowing,
because neither is obvious:

- **The commands you ask for run after your function returns**, in the order you
  asked, not at the moment you call them. Your function is already running as a
  command, and ssg finishes one command before starting the next.
- **`ssg.command` tells you the request was accepted, not that it worked.**
  Since it hasn't run yet, there's nothing to report. If one of them fails, ssg
  reports that failure against the key you pressed, names the command that
  failed, and skips the rest. If your own function raises an error, none of the
  commands it asked for run at all.

There's also a limit -- a few dozen -- on how many commands one of your
commands may ask for. Past it the call is refused rather than ssg locking up.

## Terminal capabilities

At startup ssg asks your terminal what it supports -- synchronized output,
the keyboard protocol, clipboard access -- and adapts. Nothing waits for the
answers, so a terminal that stays silent simply gets the conservative
rendering rather than a slow start.

To see what ssg decided about your terminal:

```
ssg --capabilities
```

It prints the resolved colour depth and one line per capability, then exits
without opening the editor.

Terminals sometimes claim a capability they render badly, or omit one they
actually have. Every answer can be overridden, and an override always beats
what the terminal reports:

- `SSG_TERM_SYNCHRONIZED_OUTPUT` -- tear-free full-frame redraw.
- `SSG_TERM_KEYBOARD_PROTOCOL` -- disambiguated key reporting.
- `SSG_TERM_CLIPBOARD_WRITE` -- copying to your system clipboard.

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

## Sandbox notes

`init.lua` runs in a restricted Lua interpreter: no file I/O, no
`os`/`io`/`package` libraries, no way to load other Lua files, and a
bounded instruction/time budget. This isn't a security boundary against a
malicious file (it's your own machine-local file, and you granted it
whatever it does), it's there so a runaway or accidental infinite loop in
your config can't hang ssg's startup.
