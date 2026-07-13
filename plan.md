# plan: runnable browser application

Feature spec: `doc/features/runnable-browser-application.md`

## steps

- [x] Add loopback binding, selected-port reporting, and `.mjs` MIME support to `../http`
- [x] Add production `EditorRuntime` ownership and construction
- [x] Bind and oracle-test core editing and file command families
- [x] Bind and oracle-test presentation and navigation command families
- [x] Bind and oracle-test language-service paths
- [x] Aggregate live snapshots and prove complete behavioral catalog coverage
- [x] Add shared-server `HttpEditorRoute`
- [x] Add CSPRNG bearer authentication and browser fragment handling
- [>] Add/install `ssg-editor` with asset discovery and signal-safe shutdown
- [ ] Run real-workspace direct/TUI/browser parity
- [ ] Rewrite the README around the normal-user launch path
- [ ] Run release, sanitizer, browser, consumer, and performance gates
- [ ] Complete Opus code review and fold warranted findings

## completion criteria

`ssg-editor [CWD]` prints one authenticated loopback URL, serves the bundled
browser and `/session` on the same port, edits real files through the production
runtime, and passes every gate in the feature spec.
