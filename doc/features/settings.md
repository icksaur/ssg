# spec-settings

## Goals

Provide typed, persistent, reversible settings for ordinary editor behavior without moving policy into clients.

## Design

`SettingsModel` resolves default, user, workspace, language, and document scopes in increasing specificity. Values are validated typed variants. The required key domains are:

- indentation width: integer `[1, 16]`; indentation style: `spaces` or `tabs`;
  indentation detection and automatic indentation: Boolean;
- line ending: `lf`, `crlf`, or `cr`; final newline and word wrap: Boolean;
- encoding: `utf8`, `utf8_bom`, `utf16le`, `utf16be`, `windows1252`, or
  `iso88591`;
- theme and keymap: non-empty identity strings;
- search case sensitivity, whole-word matching, and regular-expression mode:
  Boolean;
- undo and recovery byte budgets: unsigned 64-bit integers; typing-coalescing
  duration: unsigned 32-bit milliseconds.

The public types reuse `IndentStyle` and `LineEnding` from `config.h`; mixed line
endings are observable document state, not a selectable setting. A setting key
accepts only its declared value alternative.

The model takes host-provided user-configuration and workspace-storage roots so
headless tests and embedders do not depend on process-global environment
discovery. User settings persist in the user configuration root; workspace
settings use host storage keyed by canonical CWD and are not written into the
workspace. This task defines serializable language/document scope records;
later session-recovery assembly owns storing and restoring those records.

Normative commands owned by this feature:

- `settings.open`, `settings.set`, `settings.reset`, `settings.reset_scope`, `settings.export_workspace`, `settings.import_workspace`

`SettingsCommandSet` immutably enumerates all six owned IDs exactly once. This
task establishes their descriptor ownership and implements the reversible
model operations used by `settings.set` and `settings.reset`. Downstream session
assembly binds the command handlers and aggregates settings view/delta state;
the later `prompt-status-surface` owner renders prompts and status actions.

`settings.set` and `settings.reset` apply immediately after validation and
return a bounded compensation record that restores the prior scoped presence
or value. They publish typed view-state/delta data for downstream session and
status assembly. This task has no dependency on those downstream surfaces.

`settings.open` is the global configuration escape hatch. Downstream runtime
assembly binds it before focus- or mode-specific commands and opens a
server-described settings prompt with a focused input in every client state,
including an empty workspace, an existing prompt, read-only/diff content, and
distraction-free mode. Opening settings replaces an existing prompt and exits
distraction-free projection so the input is visible; it does not mutate
documents. Keymap selection or mutation is accepted only when the selected
server-owned keymap retains at least one global browser-deliverable
`settings.open` sequence.

## Invariants

I4, I16, I17, I18, I19, I20, I21, I22, I24 from `doc/spec.md`.

## Considerations

- Syntax packages provide default comment/bracket/indent rules, not mutable hidden state.
- Theme settings choose a theme identity; literal colors remain forbidden outside `Theme`.
- Invalid or unknown keys fail without changing effective settings.
- Configuration input is a server-described prompt; clients do not invent a
  settings form or retain unsent authoritative values.

## Risks and Mitigations

- Scope confusion: expose effective value plus source scope in snapshots.
- Configuration drift: round-trip versioned schema and preserve unknown future
  fields without applying them. A load-save test injects an unknown field and
  proves both non-application and byte-for-byte field preservation.

## Acceptance (Definition of Done)

- Observable: the global settings binding opens focused server-described
  configuration input from every enumerated state; changing each required
  setting updates the relevant API view/behavior and is reversible without a
  dialog.
- Budgets: settings resolution is bounded by the five fixed scopes.
- Gates: project build, settings tests, and Linux/Windows persistence tests are green.
- Oracles: table-driven global settings-open transitions, including prompt
  replacement and distraction-free exit; keymap-lockout rejection;
  hand-authored scope-resolution tables; exact/no-duplicate ownership
  of all six normative command IDs, schema round trips, invalid-value
  atomicity, restart persistence, and compensating-command restoration.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Define typed keys, values, scopes, view/delta values, and resolution | `include/ssg/settings.h`, `src/settings.cpp`, `tests/test_settings.cpp` | hand-authored resolution tables | I4, I16 |
| 2 | Implement host-rooted, versioned Linux/Windows persistence seams | `src/platform/{linux,windows}_settings.cpp`, `tests/test_settings_persistence.cpp` | schema/restart round trips plus unknown-field preservation/non-application | I18, I21 |
| 3 | Export all six immutable command descriptors and implement reversible set/reset model operations; leave handler binding, prompt rendering, and aggregate/Lua wiring to downstream owners | `include/ssg/settings.h`, `src/settings.cpp`, `tests/test_settings.cpp`, `cmake/components/settings-model.cmake` | exact command-ID cardinality/order/no-duplicates, invalid-value atomicity, and scoped-state restoration scripts | I19, I20 |

## Rationale (optional, skippable)

Settings are a cross-cutting API concern and need one owner rather than duplicated configuration in editing, presentation, and clients.
