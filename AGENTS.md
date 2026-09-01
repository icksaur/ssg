# SSG

SSG is a C++20 editor library with a terminal client. The library owns
editor behavior and authoritative product state. Each client owns presentation,
native layout, and device I/O. The library may provide optional presentation
services, but no client-specific representation is the product contract.

The code is the truth. Temporary design documents help ship uncertain work; they
are not the maintained description of a stable feature.

## Code not to write

- Do not implement editor behavior, product policy, or defaults in a client,
  transport, renderer, or platform adapter.
- Do not create a second behavior path for another client. All client input must
  converge on the same typed authoritative transitions.
- Do not make cells, pixels, resolved rectangles, terminal capabilities,
  sockets, or transport framing part of the editor-core contract.
- Do not add a feature-specific transport verb or client/server side channel
  when the typed client API can carry the same command, state, or update.
- Do not duplicate an inventory, mapping, default, label, or semantic conversion
  across the library and clients. Publish the fact or generate its consumers.
- Do not use a generic property bag where named typed fields can express the
  supported vocabulary.
- Do not let client input establish identity, capabilities, or authorization.
  The host supplies those from policy.
- Do not make pointer input the only route to an action. User-visible actions
  must remain keyboard reachable through the authoritative keymap.
- Do not add blocking confirmation UI. Destructive actions are immediate and
  recoverable through recovery, reopen, backup, or a compensating command.
- Do not invent colors outside the theme's semantic roles. Medium-specific color
  conversion belongs at the presentation edge.
- Do not introduce platform behavior outside an adapter boundary. Linux and
  Windows are required targets.
- Do not keep an old and a replacement path without a named compatibility
  requirement and a removal condition.

If a proposed abstraction makes a client reproduce product knowledge, round-trip
routine interaction that it can resolve from published data, or consume another
client's geometry, the abstraction is wrong.

## Where contracts live

Put each promise in the strongest executable form that can hold it:

1. **Types and ownership.** Prefer valid construction, strong domain types,
   private mutation, RAII, move-only ownership, and closed enums.
2. **Tests.** Pin observable behavior, boundary rejection, ordering, wire bytes,
   persistence, and independently knowable results.
3. **`// CONTRACT` comments.** Use only for a required prohibition, external
   constraint, rejected alternative, or non-local obligation that neither a type
   nor a focused test can communicate.

Attach a CONTRACT comment to the public type or function that owns the promise.
Name symbols rather than file locations or numeric values. Do not restate code,
types, or tests in prose.

## Specifications

Read the relevant code before deciding that design work is needed.

Write a temporary spec when the change has a meaningful unresolved design
choice, changes a public or wire contract, changes ownership or authority,
crosses several established seams, or can lose user data. Do not write one for
mechanical work, a local bug fix, or an implementation that follows an existing
typed seam.

Keep an active spec at `doc/specs/<slug>.md`. Review it once, implement it, and
promote the durable result into types, tests, and the small residue of CONTRACT
comments. Delete the spec when the feature is stable. Git retains the decision
history.

Plans, review transcripts, investigation logs, and status diaries are working
artifacts, not maintained project documentation. Keep them out of the repository
unless they have a continuing reader and the user explicitly wants them kept.

## Documentation

Maintained documentation must serve a current reader:

- user and configuration guides;
- public embedding and protocol contracts not fully expressed by headers;
- project-wide engineering constraints that cannot be compiled;
- short records of durable external or operational knowledge.

Do not maintain prose descriptions of internal architecture that can drift from
the code. Prefer a public header that exposes the seam and a test that exercises
it.

## Tests

- Test each behavior once at the narrowest stable seam that owns it.
- Use an independent oracle only when the expected answer exists independently
  of the implementation, such as Unicode, encoding, protocol bytes, recovery,
  atomic file operations, or a reproduced bug.
- Do not golden presentation taste, internal structure, inventories, or counts.
- A behavior-preserving refactor needs no new test when an existing focused test
  would fail on regression.
- Do not add a test framework; tests are standalone executables using
  `tests/test_helpers.h`.

## Orientation and workflow

Start with the public headers in `include/ssg`, then follow their implementations
and focused tests. Read `cpp-values.md` for public C++ value design and
`code-quality.md` for the implementation bar.

Use the smallest gate that covers the change:

```sh
scripts/check.sh
scripts/check.sh push
```

Run the fast gate for ordinary changes. Use the push tier and targeted
performance, sanitizer, platform, or protocol checks when the changed risk
requires them. Request one code review after the relevant gates pass.
