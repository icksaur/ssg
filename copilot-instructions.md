# Copilot instructions

Read `doc/spec.md`, `doc/learnings.md`, `code-quality.md`, and
`cpp-values.md` before changing SSG.

Match process to risk.

- **Tier 0 — mechanical:** behaviour-preserving refactors, generated metadata,
  and commands fitting an existing seam need no specification. Review the
  implementation once.
- **Tier 1 — local behaviour:** a change inside one established class or seam
  gets a short acceptance note in its owning feature document and one
  implementation review. No separate specification review unless a public
  contract, persistence format, security boundary, or ownership rule changes.
- **Tier 2 — contract or architecture:** new public APIs, ownership seams,
  wire/persistence formats, platform adapters, capability changes, and anything
  that can lose user data get specification, specification review,
  implementation, and code review. Fold warranted specification-review findings
  before implementation.

A specification is a current contract, not a task diary. Cap it at ~80 lines.
Move delivered history to Git; never append review transcripts, perturbation
logs, or repeated status sections.

## Cross-cutting invariants

- The C++ library is the authoritative source of editor, workspace, command,
  and view-model state.
- SSG is keyboard-first. Every user-visible action must be operable using
  browser-deliverable keyboard input. Editor commands use the authoritative
  keymap. Capability-gated platform ingress that requires browser-owned payload
  selection, such as choosing a local file, uses a server-described focusable
  control that is keyboard invokable; pointer interaction may supplement but
  never replace keyboard access.
- The server owns themes, configuration, keymaps, layout, UI elements, and
  editor behavior. Clients are thin input-and-rendering adapters and MUST NOT
  invent UI elements, product behavior, state, or defaults.
- Every client MUST expose a server-owned configuration input through an
  authoritative browser-deliverable global key binding that works in every
  client state. Configuration changes that remove all such bindings are
  invalid.
- Every UI element occupies server-described cells on the shared monospace
  grid. Clients MAY style those elements for platform readability without
  changing geometry, semantics, or behavior, and every visible color MUST come
  from the active exactly-16-color server theme.
- Every interaction enters through the typed client API and every observable
  view leaves through snapshots or deltas on that API. The complete product
  must work over one ordered WebSocket connection; do not add an out-of-band
  UI, filesystem, clipboard, status, or control channel.
- The backend implements editor and UI semantics, not rendering or platform
  input. Browser, TUI, and desktop clients capture input, map server-published
  keymaps, and render API view models on the server-described grid.
- Do not add a feature unless a current standards-based web browser can expose
  it through the client API. Browser sandboxing may require a backend service,
  but the browser client must retain the complete workflow.
- Linux and Windows are required platforms. Platform services use adapters and
  must have parity tests on both platforms.
- `Theme` is the single source of truth for all colors. The exactly 16 indexed
  colors and semantic role mappings flow through the API; clients, plugins,
  syntax definitions, and adapters may not introduce literal or computed
  colors.
- Do not use blocking dialogs or confirmation modals. Commands take effect
  immediately, report status through the view model, and make destructive or
  state-losing major actions reversible through recovery, reopen, backup, or a
  compensating command.
- Register user-visible actions in the command registry. Except for lifecycle,
  transport authentication, raw platform I/O, and capability-grant decisions,
  actions must be callable through the versioned Lua API.

## C++ and code quality

- Prefer correctness, maintainability, simplicity, then measured performance.
- Follow `cpp-values.md`: RAII, explicit caller-owned lifetime, move-only
  resource owners, valid construction, strong domain types, scoped enums, and
  separate configuration from operation.
- Keep one behavior path. In-process and WebSocket clients call the same command
  implementation and consume the same snapshot/delta model.
- Make invalid states unrepresentable and reject invalid boundary input with
  typed, actionable errors. Never return success-shaped defaults.
- Keep optional transports, renderers, Lua, LSP, Tree-sitter, and platform
  services out of the basic editor's initialization path.
- Comments explain rationale, external contracts, or non-local constraints.
  Improve names and types instead of narrating code.

## Tests

- Test each behaviour once, at the narrowest stable seam that owns it. Prefer
  class-level tests with explicit inputs and observable outputs.
- Every test file declares its kind in a header comment: **contract** (external
  truth: wire bytes, Unicode, encodings, platform), **algorithm** (independently
  knowable answer), **seam** (an architectural rule), or **smoke** (production
  composition works at all).
- Write an independent oracle before implementation only where the answer is
  knowable independently of the implementation: Unicode/layout maths,
  transactions, selection and history state machines, encoding and protocol
  bytes, recovery, atomic file operations, or a reproduced bug. Prefer
  properties, round-trips, hand cases and fault injection over writing a second
  implementation.
- Do not create an oracle, golden, or full-stack script for presentation taste,
  internal structure, inventories, or counts. For configurable presentation,
  assert that output follows configuration and satisfies bounds — never that it
  equals today's appearance.
- A golden requires an external or pinned contract, or recorded owner approval.
  Every golden has a regeneration path. Appearance is not a contract.
- A behaviour-preserving refactor needs no new test when existing focused tests
  would fail on a regression. Add a test only for a discovered gap.
- Facts live in one place. If adding a command, field, or glyph requires editing
  a list that duplicates another list, delete the duplicate instead.
- Every public command has tests for the outcomes that carry risk for that
  command: successful transition, invalid input, stale revision, failure
  atomicity, and recovery where applicable. Do not repeat the same transition at
  class, runtime, transport, and TUI layers.
- Keep one representative in-process/WebSocket parity script and one TUI
  transport smoke. Broad tests cover registration and transport, not every
  command permutation.
- Keep Linux and Windows parity tests at platform adapter boundaries, and the
  required browser conformance tests for browser input and clipboard contracts.
  Do not repeat platform-independent editor behaviour per platform or browser.
- Do not install or use Wine for Windows validation. Local task work performs
  source-level best-effort checks and may use an already-installed
  cross-compiler; native Windows CI is authoritative for Windows runtime parity.
- Each test file is a standalone executable using `tests/test_helpers.h`; do not
  add a test framework.
- Before review, build the affected target and run its focused suite. Run the
  fast gate for ordinary changes; run push, sanitizer, platform, browser, or
  performance gates only when the changed risk requires them.
