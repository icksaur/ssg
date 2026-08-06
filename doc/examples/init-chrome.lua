-- Sample init.lua chrome composition (doc/spec-lua-widget-composition.md).
--
-- Copy any of this into your own init.lua (see doc/config.md for where that
-- lives on your OS). `ssg.chrome` replaces the built-in header and/or footer
-- with your own row of widgets; omit a region to keep its built-in chrome.
--
-- Providers are live built-in values resolved every frame with NO Lua on the
-- render path: `path` (working directory), `branch` (git branch), `status`
-- (status message), `follow` (follow-edits state). A `provider` widget also
-- inherits that value's built-in click command (e.g. `path` opens the file
-- panel) unless you give it your own `command`.

ssg.chrome{
  -- The header is left-group only: the picker input line owns its right side.
  header = {
    left = {
      { kind = "field", provider = "path" },
      { kind = "field", provider = "branch" },
    },
  },

  -- The footer supports left / center / right groups.
  footer = {
    left = {
      { kind = "field", provider = "status" },
    },
    center = {
      kind = "label",
      text = "— ssg —",
    },
    right = {
      -- A static label in the footer colour role.
      { kind = "label", text = "RO", role = "footer" },
      -- A clickable "button" is just a field with a command: clicking it runs
      -- file.save, exactly as a keybinding or the palette would.
      { kind = "field", text = "Save", command = "file.save" },
    },
  },
}
