# plan

## Current: prepare for public release

- [x] Visitor-facing README; developer material split to `development.md`
- [x] Portable PII scanner (`../scan-pii.js`); scrub absolute paths from docs
- [x] Remove dead browser CI/build artifacts (browser.yml, preset/ci filters)
- [x] Remove authentication: delete `ApplicationAuthentication` + credential
      wire; credential-less `HttpEditorSessionHost::attach()`; scrub auth from
      all specs/tasks. The capability/principal authorization model is retained.
- [x] Resolve browser scope (decision: protocol stays browser-capable, no
      shipped client). Scrubbed `ssg-editor`/bundled-client claims from
      `doc/spec.md` and marked `doc/features/runnable-browser-application.md`
      historical (retained as the `EditorRuntime` design record). Kept the I18
      browser-feasibility invariant and the browser-deliverable protocol framing.
- [ ] Add a LICENSE file before publishing.

## Done: "boring OOP" architecture refactor (R1–R4)

Merged to master. Folded free functions onto value objects (R1), renamed
misnamed types/files (R2), consolidated parser factories + added
`SyntaxModel::parse` (R3), and relocated the genuine runtime split-logic
(stateless transforms onto values, the panel↔tree binding onto `TreeModel`,
`activeLiveDiffTab`/`payloadAs` dedup) (R4). The plan's ceremony controllers
(Diff/Follow/etc. holding only `Impl&`) were declined as won't-do: they own no
state the existing models don't already own.

## Reference

Reviewer session: `caco-session:d2efc7e8-...` (Opus 4.8). Commits are facts-only,
no trailer. Gate: `bash scripts/check.sh`. Config path: `~/.config/ssg/init.lua`.
