# plan

## Current: prepare for public release

- [x] Visitor-facing README; developer material split to `development.md`
- [x] Portable PII scanner (`../scan-pii.js`); scrub absolute paths from docs
- [x] Remove dead browser CI/build artifacts (browser.yml, preset/ci filters)
- [x] Remove authentication: delete `ApplicationAuthentication` + credential
      wire; credential-less `HttpEditorSessionHost::attach()`; scrub auth from
      all specs/tasks. The capability/principal authorization model is retained.
- [ ] Resolve browser scope: the browser reference client and the `ssg-editor`
      bundled-client app were removed (commit 96bfe4c5). `doc/spec.md` and
      `doc/features/runnable-browser-application.md` still describe them and weave
      "browser client" in as a design invariant (I18 feasibility, browser-safe
      keymaps). Decide: protocol stays browser-capable with no shipped client, or
      browser is fully out as a target client. Then reconcile spec.md + the
      runnable-browser feature doc.
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
