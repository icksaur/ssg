# Copilot instructions

Read `doc/spec.md`, `doc/learnings.md`, `code-quality.md`, and
`cpp-values.md` before changing SSG.

Use the workflow: specification, specification review, implementation, code
review. Do not begin implementation before warranted specification-review
findings are folded into the specification.

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

## Unit tests

- Write the strongest independent oracle before non-trivial implementation:
  reference implementation, hand-computed fixture, golden, property, or
  round-trip as specified in `doc/spec.md`.
- Every public command has unit tests for its successful transition, invalid
  input, stale revision, failure atomicity, and undo/recovery behavior where
  applicable.
- Test API seams, not only helpers. Run the same command scripts through the
  in-process API and WebSocket codec and compare canonical semantic state.
- Add Linux and Windows tests for every platform adapter. Browser-facing input
  and clipboard contracts require Chromium, Firefox, and WebKit conformance.
- Do not install or use Wine for Windows validation. Local task work performs
  source-level best-effort checks and may use an already-installed
  cross-compiler; native Windows CI is authoritative for Windows runtime parity.
- Each test file is a standalone executable using `tests/test_helpers.h`; do not
  add a test framework.
- Before review, run the configured build, `ctest --test-dir build
  --output-on-failure`, relevant sanitizer tests, and applicable platform or
  browser conformance gates.
