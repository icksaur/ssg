# SSG

SSG: a C++20 library that is the authoritative source of a terminal text
editor — documents, editing, workspaces, commands, themes, and view models. The
`ssg` terminal app and the WebSocket server adapter are thin hosts over it. The
library owns the truth; a host captures input and renders a server-described
grid.

The code is the truth. Specs are scaffolding — useful while a decision is being
made, mud once it is. What survives a change is the type that makes the bug
unrepresentable, the test that pins the behavior, and — only when neither can —
a CONTRACT line. Everything else belongs in git history, not the working tree.

## Where a contract lives — strongest first

Push every promise to the strongest bucket that can hold it. Move down only when
the bucket above genuinely cannot express it.

1. **The type system and language.** C++ can enforce far more than a comment can
   assert. A private member no caller can touch, a move-only owner, a `const`
   method, a scoped enum, a strong domain type, a constructor that admits no
   invalid object — these *are* the contract, checked at every build. Prefer
   making an invalid state unrepresentable over documenting that it is invalid.
   This bucket is why SSG needs fewer CONTRACT lines than a Go project would:
   most of what prose would promise, a type here proves.
2. **A unit test.** When the promise is a behavior — a state transition, a
   boundary rejection, a wire byte, an ordering — a test named for the rule
   holds it. The test's name is the contract; see below.
3. **A `// CONTRACT` comment.** Only for a promise that no type can encode and no
   test name can carry. This is the small residue, not the default.

## Triaging a comment

Before writing a comment, spend it upward:

1. **Turn it into a name.** A comment describing *what* the code does means the
   names are too weak. Rename the file, type, function, or variable until the
   comment is redundant, then delete it. Most comments die here.
2. **Keep it only as a "why".** An external constraint, a non-local coupling, a
   rejected alternative, a deliberate refusal — knowledge not visible in the
   code itself. If the "why" is a load-bearing promise, it is a CONTRACT line.

Do not narrate code. Do not restate what a type already guarantees.

## When to write a spec

First read the relevant code. Not knowing where something lives, or how it
currently works, is never a reason to write a spec — it is a reason to read.
Orient first, then answer both questions.

1. Is there a real design choice here — two or more workable approaches whose
   consequences differ, which reading the code cannot settle because the answer
   does not exist yet and must be decided?
2. Does the change leave every existing CONTRACT line still true?

If no to 1 and yes to 2: **write no spec.** Read the CONTRACT lines at the site,
change the code and its tests, run the gates, request one code review.

If yes to 1 or no to 2: **write a spec** at `doc/specs/<slug>.md`. Get one review
covering design and plan together, implement it, promote the durable residue
into types, tests, and CONTRACT lines, then delete the spec file in the same
commit. Git keeps the record permanently; the working tree keeps only documents
with a live reader.

In a clean tree, `doc/specs/` is empty. A spec exists only between the start of
design and the merge of the feature it describes. Nothing survives it except the
code, its types, its tests, and any CONTRACT line — its plan and file list are in
git, and its exposition had no reader after the review. If part of a finished
spec fits none of those destinations, that is evidence it was never load-bearing.

Recover a deleted spec:
`git log --all --diff-filter=D --name-only -- 'doc/specs/*<slug>*'`
then `git show <sha>^:<path>`.

Question 1 is about design uncertainty, not familiarity. A spec earns its cost
when the decision has options and the options have consequences: fitting a new
technique into an existing pipeline under a budget, a concern coordinated across
many call sites, a mechanism with a plausible alternative worth recording as
rejected. If reading the code makes the change obvious, it is transcription — go
write it.

Question 2 asks whether the change alters a promise something else relies on: the
shape or meaning of a wire message, who owns or may mutate a piece of state, the
lifetime of a value a caller holds, an ordering or concurrency rule, or a
documented refusal. Changing a value, adding a case, or restructuring code behind
an unchanged promise is not such a change. If a CONTRACT line becomes wrong
because of your change, that is the signal.

## CONTRACT lines

A test states that behavior holds. It cannot state that the behavior is
*required* — a red test looks the same whether you broke a promise or outgrew a
fixture, and an agent under pressure will edit it either way. A CONTRACT line
exists to supply that missing authority, and nothing else.

Write one only when all four are true:

1. No type already enforces it. If a private member, a `const`, a move, or a
   stronger domain type would make the violation not compile, do that instead.
2. No test name already says it. A test named for the rule it enforces is the
   contract; restating it in prose adds a second copy that can rot.
3. No better name says it. If renaming a symbol would carry the meaning, rename
   the symbol.
4. Something would plausibly do the wrong thing without it.

That leaves a small set: a deliberate refusal that looks like an unimplemented
feature, a prohibition on code nobody has written yet, a rejected alternative, an
external constraint invisible in the tree, an emergent property no single
function implies. **The library/app boundary is the richest source of these** —
what a host may assume about the library, what the library refuses to do on a
host's behalf, who owns a buffer's lifetime across the seam. Capture those on the
public header that owns the seam.

```cpp
// CONTRACT
// HttpEditorSessionHost: the host is the sole authority for a connection's
//   principal and capabilities; it never derives them from client-supplied
//   input. A credential-less attach is deliberate, not an omission.
```

- Every line starts with the artifact name, so a truncated read is still true.
- Attach to a party — something with behavior that can keep or break a promise.
  A constant or a plain struct field cannot; the contract belongs on the function
  or class that enforces it.
- An invariant spanning two values is a test, never a comment. Prose cannot hold
  an inequality, and no single artifact owns it.
- No values in prose. Name the symbol, never the number.
- No file paths and no line numbers. Symbol names survive edits; locations do not.
- Do not write "enforced by test". Either the test's name says the rule, in which
  case the line is redundant, or it does not, in which case fix the name.
- Whole-repo budget: 150 lines. `rg '^// CONTRACT' -A 20` must fit in context.

Name a test for the rule it enforces, not the area it covers. `Never`, `Only`,
and `Always` are load-bearing words: a test named for a broken-promise failure
tells a reader that a red bar means a contract broke, not a fixture drifted.

## Global rules

Rules with many consumers and no owning artifact. **This list is a defect list.**
Each entry is knowledge that could not find a home in a type, a test, or a
CONTRACT line, so it must be carried in prose and read by everyone. Burn it down:
when a type, a chokepoint, or a test can own a rule, move it there and delete the
entry.

Do not add, reword, or remove an entry on your own initiative — ask the user
first. A change here alters what every future session is told.

- The library is the authoritative source of editor, workspace, command, theme,
  and view-model state; a host invents no UI element, product behavior, state, or
  default. → wants the seam narrow enough that a host physically cannot.
- Every interaction enters through the typed client API and every observable view
  leaves through a snapshot or delta on it; there is no out-of-band UI,
  filesystem, clipboard, or control channel. The whole product works over one
  ordered WebSocket connection. → wants a chokepoint on the API surface.
- `Theme` is the single source of all color: exactly 16 indexed colors and
  semantic role mappings flow through the API. No client, plugin, syntax
  definition, or adapter introduces a literal or computed color. → wants a lint
  over the render path.
- A connection's identity and capabilities come only from the host's policy,
  never from a client-supplied field. → wants a chokepoint at the attach seam.
- In-process and WebSocket hosts call the same command implementation and consume
  the same snapshot/delta model. There is one behavior path. → wants the duplicate
  path to be impossible to write, not merely absent.
- Linux and Windows are required; platform services use adapters with parity
  tests on both. → wants the adapter seam to be the only platform-specific site.
- Values and budgets live in code; prose names symbols, never numbers. → wants a
  lint over CONTRACT lines and Markdown.

## Orientation

`rg '^// CONTRACT' -A 20` enumerates what this repository promises. That output
is the map; it cannot go stale, because it is the thing itself. Start there, then
`ls include/ssg` for the public library surface and `ls apps` for the hosts over
it — the boundary between them is where the load-bearing contracts live.

`cpp-values.md` states the C++ value system (RAII, caller-owned lifetime,
move-only owners, valid construction, strong domain types, scoped enums,
configuration separated from operation). `code-quality.md` is the short bar.
Read them before changing SSG; they are the standing "how", not per-feature specs.

## Gates

```
scripts/check.sh          # build + fast unit tests — the edit-test loop
scripts/check.sh push     # adds the correctness oracle + embed-consumer build
```

Run the fast gate for ordinary changes; run the `push` tier before pushing, and
`perf`, sanitizer, or platform gates only when the changed risk requires them.
Before review, build the affected target, run its focused suite, and make
`rg '^// CONTRACT'` still true.

Every test file declares its kind in a header comment — **contract** (external
truth: wire bytes, Unicode, encodings, platform), **algorithm** (an independently
knowable answer), **seam** (an architectural rule), or **smoke** (production
composition works at all) — and is a standalone executable using
`tests/test_helpers.h`. Do not add a test framework. Write an independent oracle
before implementation only where the answer is knowable independently of the
code: Unicode/layout math, history and selection state machines, encoding and
protocol bytes, recovery, atomic file operations, or a reproduced bug. Do not
golden presentation taste, structure, or counts.
