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
    sequence = "Escape KeyF KeyQ",
    command = "file.save",
})
ssg.command("keymap.unbind", {
    sequence = "Escape KeyS",
})
```

- `sequence` is one or more key strokes separated by spaces, e.g.
  `"Escape KeyF KeyQ"` (press Escape, then F, then Q) or `"Ctrl+Shift+KeyM"`
  (a single chord). Each stroke is an optional `Ctrl+`/`Alt+`/`Meta+`/
  `Shift+` prefix followed by one key name: `KeyA`-`KeyZ`, `Digit0`-
  `Digit9`, `F1`-`F24`, or a named key (`Escape`, `Enter`, `Tab`, `Space`,
  `Backspace`, `Delete`, the arrow keys, `Home`/`End`/`PageUp`/`PageDown`,
  and punctuation names like `BracketLeft`/`Comma`/`Slash`).
- `command` is the command id to run (the same ids used throughout ssg,
  e.g. `file.save`, `edit.undo`, `tab.next`).
- `context` is optional and defaults to `"*"` (every focus target); it can
  instead be `"editor"`, `"panel"`, or `"prompt"` to bind only while that
  part of the UI has focus.
- `keymap.bind` replaces any existing binding for the same
  `(sequence, context)` pair rather than adding a duplicate. It's rejected
  -- leaving the keymap unchanged -- if the sequence is unparseable, the
  context is unknown, `command` is empty, or the result would be an
  invalid keymap (e.g. an ambiguous prefix, or removing the last
  `Escape KeyF KeyT` -> `settings.open` binding, ssg's built-in escape
  hatch to the Settings screen).
- `keymap.unbind` removes any binding matching `(sequence, context)`;
  unbinding something that isn't bound is not an error. It's rejected on
  the same "would remove the last `settings.open` binding" ground as
  `keymap.bind`.
- Bound commands show up in the command palette (`Escape Shift+KeyP`) with
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

## Sandbox notes

`init.lua` runs in a restricted Lua interpreter: no file I/O, no
`os`/`io`/`package` libraries, no way to load other Lua files, and a
bounded instruction/time budget. This isn't a security boundary against a
malicious file (it's your own machine-local file, and you granted it
whatever it does), it's there so a runaway or accidental infinite loop in
your config can't hang ssg's startup.
