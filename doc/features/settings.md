# spec-settings

## Goals

Provide typed, persistent, reversible settings for ordinary editor behavior without moving policy into clients.

## Design

`SettingsModel` resolves default, user, workspace, language, and document scopes in increasing specificity. Values are validated typed variants. Required keys include indentation width/style/detection, auto-indent, line-ending/final-newline policy, encoding, word wrap, theme, keymap, search options, undo/recovery budgets, and typing-coalescing duration. User settings persist in the user configuration root; workspace settings use host storage keyed by canonical CWD unless explicitly exported; language/document overrides persist with session recovery.

Normative commands owned by this feature:

- `settings.open`, `settings.set`, `settings.reset`, `settings.reset_scope`, `settings.export_workspace`, `settings.import_workspace`

Settings use the non-modal `PromptSurface`, apply immediately after validation, and expose a compensating footer action containing the prior effective value.

## Invariants

I4, I16, I18, I19, I20, I22 from `doc/spec.md`.

## Considerations

- Syntax packages provide default comment/bracket/indent rules, not mutable hidden state.
- Theme settings choose a theme identity; literal colors remain forbidden outside `Theme`.
- Invalid or unknown keys fail without changing effective settings.

## Risks and Mitigations

- Scope confusion: expose effective value plus source scope in snapshots.
- Configuration drift: round-trip versioned schema and preserve unknown future fields without applying them.

## Acceptance (Definition of Done)

- Observable: changing each required setting updates the relevant API view/behavior and is reversible without a dialog.
- Budgets: settings resolution is bounded by the five fixed scopes.
- Gates: project build, settings tests, and Linux/Windows persistence tests are green.
- Oracles: hand-authored scope-resolution tables, schema round trips, invalid-value atomicity, restart persistence, and compensating-command restoration.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Define typed keys, values, scopes, and resolution | `include/ssg/settings.h`, `src/settings.cpp`, `tests/test_settings.cpp` | hand-authored resolution tables | I4, I16 |
| 2 | Implement versioned Linux/Windows persistence | `src/platform/*settings.cpp`, `tests/test_settings_persistence.cpp` | schema/restart round trips | I18 |
| 3 | Implement command, prompt, and compensating-action integration | `data/required-commands.json`, `tests/test_settings.cpp` | invalid-value and restoration scripts | I19, I20 |

## Rationale (optional, skippable)

Settings are a cross-cutting API concern and need one owner rather than duplicated configuration in editing, presentation, and clients.
