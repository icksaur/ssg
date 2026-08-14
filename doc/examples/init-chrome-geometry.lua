-- Chrome geometry demo: a composition that exercises the sizing, spacing, and
-- role constraints the published UI-VM schema carries, so you can repro the
-- interesting layout cases by eye in either client.
--
--   cp doc/examples/init-chrome-geometry.lua ~/.config/ssg/init.lua
--   ./build/ssg .            # TUI: the schema lowers to cells
--   ./build/ssg --http 8971  # web: open http://127.0.0.1:8971/ (DOM interprets it)
--
-- The TUI and the web draw the SAME published schema, so every case below must
-- look the same in both (geometry is each client's medium; the tree is shared).
--
-- What to look for, footer left -> right:
--   * A fixed-width Spacer holds a hard gap that never collapses.
--   * A field carrying an explicit `role` colours in that semantic role, not the
--     footer default -- proof the role is resolved by the library and published,
--     not re-derived per client.
--   * A checkbox shows its box glyph and caption from a literal checked state.
--   * The center label has a fixed Exact width (`width = 20`). NOTE: the "center"
--     is the flex MIDDLE slot between the left and right groups, not a geometric
--     centerer -- a Fixed-width center sits at the START of that middle, right
--     after the left group, in BOTH clients (see Widget.cpp: a Fixed center is
--     placed at leftEnd). It stays exactly that many cells wide as the window
--     resizes. Set `width = "flex"` to make the center FILL the whole middle span
--     instead (the slack collapses into it); set `width = 0` for a true
--     zero-extent center (the Exact(0) case) that vanishes while the slack stays.
--
-- The point of the demo is fidelity, not styling: the web draws the SAME schema
-- the TUI lowers to cells, so every case above must look the same in both.

ssg.chrome{
  header = {
    left = {
      { kind = "field", provider = "path" },     -- live cwd (click opens files)
      { kind = "field", provider = "branch" },    -- live git branch
    },
  },

  footer = {
    left = {
      { kind = "field", provider = "status" },
      { kind = "spacer", width = 4 },             -- a hard, non-collapsing gap
      { kind = "label", text = "WARN", role = "status_warning" },  -- role override
      { kind = "checkbox", text = "wrap", checked = true },        -- literal checked
    },
    -- A fixed Exact center: exactly 20 cells wide, anchored at the start of the
    -- flex middle (NOT geometrically centered -- that matches the TUI). Set
    -- width = "flex" to fill the whole middle, or width = 0 for a zero extent.
    center = {
      kind = "label",
      text = "-- middle slot --",
      width = 20,
    },
    right = {
      { kind = "field", text = "Save", command = "file.save" },
    },
  },
}
