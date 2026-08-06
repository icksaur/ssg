# spec-grammar-pipeline

## Goals

Syntax highlighting keeps working when the source checkout is absent, moved, or
renamed. Adding a vendored grammar is a single data entry rather than edits scattered
across CMake source lists, include directories, an `extern "C"` declaration and
a C++ table. The project builds and tests in ONE
configuration instead of two. A host embedding SSG can still choose plain-text highlighting, and can register
grammars SSG does not vendor THROUGH A PUBLIC HEADER.

## Design

Three independently shippable phases, in order. Each leaves the tree green.

### Phase A: embed the highlight queries

Today `src/TreeSitterParser.cpp`'s `kGrammars` table stores `queryPath` as an
absolute path built from the `SSG_TREESITTER_VENDOR_DIR` compile definition, and
`queryFor()` reads that path with `std::ifstream` on first use of a language.
The shipped binary therefore depends on the source tree at RUNTIME: 54 absolute
paths appear in `build/ssg`, and `readFile` returns an empty string for a
missing file, which `queryFor` caches as a permanent failure and reports as
"no highlighting" with no diagnostic. Moving the checkout silently degrades the
editor.

Replace the runtime read with a generated translation unit. A CMake list of
`(key, file)` pairs drives a generator script that emits one `.cpp` defining a
lookup from key to `std::string_view`. `kGrammars` stores the KEY; `queryFor`
resolves it through the generated table. No `std::ifstream`, no
`SSG_TREESITTER_VENDOR_DIR`, no filesystem dependency.

Mechanism for the generated literal: a C++ raw string with a fixed delimiter.
The generator MUST fail the build if a source file contains the closing
delimiter sequence, so a future grammar cannot silently produce a malformed or
truncated TU. This is the one artifact-accommodation rule in this spec that has
no standard name: it is cheap, it is invisible when it works, and without it the
failure mode is a confusing compile error in generated code.

Extensibility requirement, scoped honestly: this makes adding a grammar's QUERY
TEXT a one-entry change. It does NOT make adding a whole grammar one line --
that still needs its `parser.c`/`scanner.c` in the source list, its include
directory, an `extern "C"` declaration, and a `kGrammars` row. Phase C is what
collapses the remainder; Phase A must not be described as solving it.

Total embedded size is ~11.4 KiB across six queries, so binary growth is not a
consideration.

### Phase B: drop SSG_TREESITTER

The compile-time flag is nearly vestigial. The `#ifdef` surface is three files
(`src/TreeSitterParser.cpp`, `src/TreeSitterParser.h`,
`src/TreeSitterParser.cpp`); nothing in the library core references tree-sitter.
The disable mechanism users actually need already exists at RUNTIME:
`EditorRuntimeConfig::syntaxParser` is a `std::shared_ptr<SyntaxParser>`, and a
null parser means plain text. All seven `tests/runtime/*.cpp` files already run
that way, setting no parser at all.

So the flag's only remaining effect is whether the vendored C is compiled. Remove
it: always compile the grammars, keep `TreeSitterParserFactory::createDefault()` as the app's opt-in
and a null `syntaxParser` as the documented way to disable highlighting.

This collapses two build configurations into one.

**Retired capability, stated plainly so the trade is intentional:** after Phase B
SSG cannot be built without tree-sitter. Every consumer -- including a project
pulling SSG in via `add_subdirectory` -- compiles the vendored tree-sitter
runtime and six grammars, and therefore requires a working C compiler
(`enable_language(C)` moves out of the `if`). Consumers who want no
highlighting still pay the build cost and get the disable only at runtime. This
is worth it here because nobody exercises the OFF configuration except the gate
itself, but it is a genuine reduction in what SSG can be embedded into, and it
is the reason Phase B is separable from Phase A: Phase A is a pure bug fix and
should land even if this trade is later rejected. Nine existing specs name `SSG_TREESITTER` in their gate and must be updated in
the same change, or they assert a configuration that no longer exists:
`spec-color-depth-defaults`, `spec-diff-default-wiring`, `spec-diff`,
`spec-diff-tint-hue-fidelity`, `spec-document-lifetime`, `spec-git-diff-source`,
`spec-inline-word-diff`, `spec-prompt-fulfillment`, `spec-syntax-and-diffs`.
Two more name the `build-no-ts` directory: `spec-config`, `spec-file-finder`.

### Phase C: grammar-registration seam

`kGrammars` is a file-local `std::array` in an anonymous namespace, holding an
`extern "C"` language function pointer per grammar. A host cannot add one.

Introduce a registration type carrying what a grammar needs -- its language
ids/aliases, its language factory, its highlight query source, and the optional
inherited query source -- and a factory taking a set of them, defaulting to the
vendored set. The vendored grammars become the default argument rather than the
only possibility.

**This type must live in a PUBLIC header, or the stated goal is unmet.**
`src/TreeSitterParser.h` is not public; a host embedding SSG cannot reach it. So
Phase C adds `include/ssg/TreeSitterGrammars.h` with the registration type and a
`TreeSitterParserFactory::create(grammars)` factory returning `shared_ptr<SyntaxParser>`.

That collides with keeping tree-sitter out of public headers, so the invariant is
stated precisely: public headers must not INCLUDE tree-sitter headers, and must
not require the host to. The language factory is therefore carried as an opaque
alias (`using SyntaxLanguageHandle = const void*`), documented as "the
`const TSLanguage*` returned by a tree-sitter grammar's `tree_sitter_<name>()`".
A host that is already linking a grammar has that pointer; a host that is not
never touches the type. The cost is real and is accepted deliberately: the
opaque alias trades compile-time type safety for a public header that pulls in no
third-party types. The alternative -- vendoring tree-sitter's headers into
`include/` -- would make every consumer of any SSG header depend on them.

Query source is passed as text, not a path: Phase A already removed path
handling, and a host may have its query in a string, a resource, or its own
generated TU.

## Invariants

- INV-no-optional-init (`doc/spec-fast-startup.md`): no optional subsystem,
  including `OptionalSubsystem::TreeSitterGrammar`, may be constructed before the
  first frame. Compiling grammars unconditionally does not change when they are
  CONSTRUCTED; the startup audit continues to enforce this and must stay green.
- The library never requires tree-sitter at RUNTIME: a null `syntaxParser`
  yields plain text. Phase B makes this the only disable mechanism, so it moves
  from "one of two" to load-bearing.
- Library-is-contract: highlighting stays behind the `SyntaxParser` seam. Phase C
  widens who can supply grammars; public headers must not INCLUDE tree-sitter
  headers, and a host that does not use grammars must not be forced to have them.
- A C toolchain becomes MANDATORY at Phase B. `enable_language(C)` is currently
  inside `if(SSG_TREESITTER)`; unconditional compilation makes the vendored C
  (tree-sitter plus six grammars) a hard build requirement for every consumer,
  including `add_subdirectory` embedders. This is the retired capability named
  below and must be an intentional trade, not a side effect.

## Considerations

- **The current failure mode is silent.** `readFile` returns `""` for a missing
  file and `queryFor` caches that as a permanent failure. Phase A removes the
  failure mode; it should not replace it with a different silent one. A key with
  no generated entry is a programming error and should be loud (build-time if
  possible).
- **`ts_query_new` does not process tree-sitter's `; inherits:` directive.** The
  existing code compensates by prepending the base grammar's query text (C for
  C++, JavaScript for TypeScript) so the derived grammar's rules override. This
  is a real artifact accommodation and must survive all three phases unchanged.
- **Query compilation is cached, including failures**, behind a mutex, keyed by
  path today. The key becomes the grammar key. The cache is global and
  process-lifetime; that is unchanged.
- **`kGrammars` aliases are matched by `LanguageId`**, with a fixed 4-slot alias
  array. Phase C should not silently keep a fixed-size alias limit if the
  registration type is meant to be host-facing.
- **Eleven docs reference the doubled gate or its build directory.** Phase B must update them in the
  same commit; a spec asserting an impossible gate is worse than no spec.

## Risks and Mitigations

- Generated-code build fragility -> generator is a small CMake script with a
  build-time delimiter check; the generated TU is compiled like any other source.
- Losing the "library builds without tree-sitter" proof -> accepted
  deliberately in Phase B, recorded here and in the affected specs. The runtime
  null-parser path remains, and is exercised by every runtime test.
- Phase C over-designing for a host that does not exist -> keep the registration
  type minimal (ids, language factory, query text, inherited query text); do not
  add discovery, plugin loading, or dynamic registration.

## Acceptance (Definition of Done)

- Observable: `ssg` highlights a C++ file correctly when the vendor query files
  are unavailable (Phase A, the decisive check); `strings build/ssg` contains no
  `vendor/tree-sitter*/queries` path (Phase A, supplemental); one build
  directory, one gate (Phase B); a host TU that includes
  `ssg/TreeSitterGrammars.h` and no tree-sitter header compiles and can register
  a grammar (Phase C).
- Budgets: no measurable startup or open-path regression; embedded size ~11.4
  KiB.
- Gates: `bash scripts/check.sh` green. After Phase B that is the ONLY gate;
  through Phase A both configurations stay green, since Phase A must be
  shippable even if Phase B is rejected.
- Oracles: below, per step.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add the query-embedding generator: a CMake list of (key, file) pairs and a script emitting one TU mapping key -> `std::string_view`, failing the build if a file contains the raw-string closing delimiter | `cmake/components/treesitter-syntax.cmake`, new `cmake/embed_text.cmake`, new generated TU in the build dir | test: each embedded key's text is byte-identical to the corresponding `vendor/**/highlights.scm` read independently by the test -- a reference-impl oracle, so a truncating or escaping generator fails | - |
| 2 | Switch `kGrammars` to hold keys and `queryFor` to resolve through the generated table; delete `readFile`, the `<fstream>` include, and `SSG_TREESITTER_VENDOR_DIR` | `src/TreeSitterParser.cpp`, `cmake/components/treesitter-syntax.cmake` | THREE layers, because no single one is decisive: (a) BEHAVIORAL, primary -- highlight every vendored language with the vendor query files made unreadable, in a temp copy of the tree so no shared state is mutated; (b) STRUCTURAL -- source scan asserting `TreeSitterParser.cpp` contains no `ifstream`/`fopen` and no `highlights.scm` (the same technique `test_theme.cpp` and `test_ssg_app.cpp` already use); (c) SUPPLEMENTAL -- `strings` on the binary finds no `queries/highlights.scm`, which is artifact- and tool-dependent and therefore cannot be the only proof. Existing `test_treesitter_syntax` green unchanged | - |
| 3 | Verify the inherits-prepending still applies with embedded text | `src/TreeSitterParser.cpp` | test: a C++ fixture highlights a construct defined ONLY in the inherited C query, failing if the prepend is dropped -- **verify by perturbation** (drop the prepend, confirm failure, revert) | - |
| 4 | **Phase A gate**: full suite green in both configurations, still | - | both gates green | - |
| 5 | Remove the `SSG_TREESITTER` option and all three `#ifdef`s; always compile grammars; `TreeSitterParserFactory::createDefault()` unconditionally returns a `TreeSitterParser` | `CMakeLists.txt`, `cmake/components/treesitter-syntax.cmake`, `src/TreeSitterParser.cpp`, `src/TreeSitterParser.h`, `src/TreeSitterParser.cpp`, `include/ssg/EditorRuntime.h` | test: a runtime built with a null `syntaxParser` produces plain-text spans (the disable mechanism, now load-bearing); startup audit green (no grammar constructed before the first frame); COMPATIBILITY -- a minimal `add_subdirectory` consumer project configures, builds and links, so the embedder path is proven still to work rather than assumed | INV-no-optional-init |
| 6 | Update the 9 specs naming `SSG_TREESITTER` in their gate to name the single gate, and record the retired claim in `doc/spec-syntax-and-diffs.md` | `doc/spec-color-depth-defaults.md`, `doc/spec-diff-default-wiring.md`, `doc/spec-diff.md`, `doc/spec-diff-tint-hue-fidelity.md`, `doc/spec-document-lifetime.md`, `doc/spec-git-diff-source.md`, `doc/spec-inline-word-diff.md`, `doc/spec-prompt-fulfillment.md`, `doc/spec-syntax-and-diffs.md` | grep: no doc references `SSG_TREESITTER` as a gate | - |
| 7 | Delete the `build-no-ts` configuration from the workflow | `doc/spec-config.md`, `doc/spec-file-finder.md`, `AGENTS.md` if referenced | grep: no doc instructs a second build dir | - |
| 8 | Add the PUBLIC registration type and `TreeSitterParserFactory::create(grammars)` factory (default argument = the vendored set), carrying the language factory as the opaque `SyntaxLanguageHandle` | new `include/ssg/TreeSitterGrammars.h`, `src/TreeSitterParser.h`, `src/TreeSitterParser.cpp`, `tests/test_treesitter_syntax.cpp` | test: a parser built with ONLY a custom registration highlights that language and does NOT highlight a vendored one (proves substitution, not merging); and the new public header compiles in a TU that includes NO tree-sitter header, proving a host is not forced to have them | library-is-contract |
| 9 | **Phase C gate**: single gate green; public headers still free of tree-sitter types | - | full suite green; grep: no `tree_sitter` in `include/` | library-is-contract |

## Rationale

The runtime source-tree read is a live defect, not a hypothetical: 54 absolute
paths are in the current binary, and the degradation is silent. That is why
embedding comes first and is independently shippable -- it is worth doing even
if the other two phases never happen.

Dropping the flag is a simplification with a real, named cost (the
dependency-free library build). It is worth taking because the flag protects a
capability nobody exercises, while the doubled gate taxes every change: of the
81 tests, 80 are identical between configurations, and only
`test_treesitter_syntax` differs.

Phase C is last because it is the only speculative one. Phases A and B pay for
themselves immediately; the registration seam pays off only when someone adds a
grammar, so it is deliberately minimal.
