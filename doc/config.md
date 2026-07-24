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

## What you can do today

The only thing `init.lua` can currently do is change palette colors:

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

## What's NOT possible yet

- **No other commands are exposed to `init.lua` yet.** `theme.define` is
  the only one. ssg has ~170 other commands (cursor movement, editing,
  file operations, etc.) but none of them are callable from Lua today --
  only whichever ones a future update explicitly adds.
- **No multiple named themes / theme switching.** `theme.define` edits the
  one active theme's colors in place; there's no way to define two themes
  and switch between them yet.
- **No hot-reload.** `init.lua` only runs once, at startup. Changing it
  requires restarting ssg.
- **No `--config <path>` flag.** The file location above is fixed (other
  than via `XDG_CONFIG_HOME`/`%APPDATA%`); there's no way to point ssg at
  an arbitrary file per-invocation yet.

## Sandbox notes

`init.lua` runs in a restricted Lua interpreter: no file I/O, no
`os`/`io`/`package` libraries, no way to load other Lua files, and a
bounded instruction/time budget. This isn't a security boundary against a
malicious file (it's your own machine-local file, and you granted it
whatever it does), it's there so a runaway or accidental infinite loop in
your config can't hang ssg's startup.
