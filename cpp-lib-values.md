# C++ Values

My values for designing and cleaning up C++ libraries. Distilled from
`vkobjects` (the most stable API and behavior I have shipped), the `audio`
library, `ssg`, and `code-quality.md`. These are decision criteria, not a
checklist to apply mechanically.

The goal: a library that is **correct by construction**, **hard to misuse**, and
**small enough to read end-to-end**. Comments are a last resort.

---

## Core philosophy

| Principle | Statement |
|---|---|
| Code is a liability | Less code = less to maintain, test, and break. |
| Correct by design | Eliminate entire classes of bugs through structure, not discipline. |
| Simple is best | Complexity is the greatest enemy; coupling is its source. |
| Strong typing | Catch errors at compile time, not runtime. |
| Pit of success | The easy way to call the API is the correct way. |

**Worst:** complexity · coupling · the wrong abstraction (expensive forever).
**Bad:** relying on side effects · global state · unnecessary layers · mutable
shared state · code that must be kept in sync.

---

## Pit of success

Design so that the path of least resistance is the correct path. A caller who
does the obvious thing should get correct behavior; a caller who does the wrong
thing should get a compile error or an immediate, descriptive throw — never
silent corruption or a sentinel that looks like real data.

Ranked by leverage — prefer higher:

1. **Make the wrong thing unrepresentable.** Types, scoped enums, private ctors,
   builders, move-only ownership. If an invalid combination cannot be spelled,
   it cannot be shipped. (`Stage` where `Access` is expected is a compile error;
   a `Buffer` that exists is a valid `VkBuffer`.)
2. **Encapsulate the contract.** One object owns a required sequence; all callers
   route through it. Replace "call A then B then C" with one named operation.
3. **Assert at the boundary.** If the type system cannot express it, throw or
   fail the test loudly and early — never return `0`/`null`/empty on misuse.
4. **Comment at the call site.** Cheap, high-decay, last resort.

A measurement/query with no error path that returns the "no-data" sentinel on a
contract violation is an implicit-coupling bug: the caller cannot distinguish
"correct empty" from "you used me wrong." Prefer a result type that can say
which.

The test of success is the **consumer**: the happy path reads in domain
language, required ordering follows from object structure, ownership and lifetime
are visible, invalid inputs fail at the boundary with actionable errors, and
comments explain decisions rather than operating instructions.

---

## Object-oriented, not dogmatic

The library is built **from objects that compose**, not from features expressed
as free-function modules. An app composes the library's objects to make a
program; the library composes its own objects to make behavior. Behavior belongs
to the object that owns the relevant state or concept.

**But avoid dogmatic OOP.** Object orientation is a tool for encapsulation and
composition, not a ceremony:

- **No getter/setter ritual.** A `struct` of public data is better than a class
  with a private field and two trivial accessors. Expose data as data; expose
  behavior as methods.
- **Encapsulation over inheritance; composition over both.** Deep class
  hierarchies and virtual-for-its-own-sake are smells. Reach for polymorphism
  only where call sites genuinely branch on a runtime type; otherwise a plain
  function or a variant is simpler.
- **Stateless logic still deserves an object,** but a small one. A pure transform
  becomes a class with verb methods on a value (`Renderer{}.render(snapshot)`,
  not free `render(snapshot)`) so it is a unit of composition, naming, and
  testing — not because "everything must be a class."
- **Pragmatic C-style is allowed, and lives in lowercase files.** Free helper
  functions are fine as `.cpp`-local statics/anonymous-namespace helpers, or
  gathered into an internal lowercase header that is explicitly *not* public
  API. `vkobjects` does exactly this: `vkinternal.h` holds free helpers
  (`createSampler`, `recordMipmapGeneration`, swapchain plumbing) shared across
  translation units — a deliberate C-style seam under the object API. The rule is
  about the **public** surface: public headers expose objects, not a bag of free
  functions.

So: objects for the public vocabulary and for anything that owns state or a
contract; free functions, pragmatically, for internal glue in lowercase files.

---

## Naming and case

| Kind | Case | Examples |
|---|---|---|
| Types (`class`/`struct`/`enum class`/aliases) | **PascalCase** | `VulkanContext`, `BufferBuilder`, `Document`, `TabManager` |
| Functions / methods | **camelCase** | `submitAndWait`, `deviceAddress`, `beginCommands` |
| Variables / fields | **camelCase** | `windowWidth`, `swapchainImageViews` |
| Private data members | **trailing underscore** | `rid_`, `handle_`, `graph_` |
| Shared constants | **kCamelCase** | `kNullRid`, `kCacheMagic` |
| Capacity-style constants | **UPPER_SNAKE** | `MAX_STORAGE_BUFFERS` |
| Macros | **UPPER_SNAKE** | (mostly vendor/integration macros) |

**Names are accurate nouns.** All classes, structs, members, and files are
nouns. If you wrote a comment explaining what something *is*, the name is wrong —
rename instead. If a type's purpose drifted (a `Synth` that no longer
synthesizes), rename to match what it does today. Names communicate *what* and
*why*, never *how*.

### Filenames

A filename predicts what is inside it:

- **A class-defining file is named for its one primary object, in PascalCase**
  (`Document.cpp` defines `Document`; `TabManager.cpp` defines `TabManager`).
  Builders, small collaborators, and value types used only by that object may
  share the file — colocating a builder with its product is easier to reason
  about than scattering it.
- **Lowercase snake_case is reserved for files that are not a single class:**
  utility/value-bag headers (`types.h`, `color.h`, `config.h`), functional-core
  or codec modules whose export is a family of related operations, the umbrella
  header, `main`-bearing files, and the pragmatic C-style internal helper files
  described above (`vkinternal.h`).

> Note: `vkobjects` predates this convention and uses lowercase implementation
> files (`buffer.cpp` implementing `Buffer`). `ssg` establishes the
> PascalCase-class-file rule going forward. Either is internally consistent; new
> work follows the PascalCase-class-file rule, and a mixed tree is not worth a
> churn commit on its own.

---

## struct vs class

- **`struct`** for plain data/config holders, lightweight builders/state
  carriers, POD-ish interop types. (`VulkanContextOptions`, `BufferBuilder`,
  `BindlessTable`.)
- **`class`** for resource-owning RAII wrappers and types with private
  invariants or lifetime rules. (`VulkanContext`, `Buffer`, `Image`, `Document`.)

Small POD value structs and their `operator==` / aggregate init are not
"behavior" and stay plain.

---

## Formatting

- 4-space indentation, no tabs.
- K&R braces (opening brace on the same line).
- One space between type and name. **No vertical alignment** of types and
  variable names — it churns on every rename and obscures diffs.
- Small one-liner methods may stay inline when clear.
- `.cpp` files start with project/internal includes, then STL/system includes.
- File-local helpers go in an anonymous namespace in the `.cpp`.

---

## Header and source organization

- **Public API lives in `include/`, implementation in `src/`.** `src/` may
  contain private headers (e.g. `src/dsp/`, `src/vkinternal.h`) the public
  surface must never expose. If a type is only used by implementations, it does
  not belong in `include/`.
- **One umbrella header per library** (`vkobjects.h`, `Audio.h`, `ssg.h`).
  Application code writes one `#include` and gets everything; facet headers stay
  independently includable for callers who want a smaller footprint.
- `#pragma once`, always.
- **Helpers live in `.cpp` files, not headers.** Headers expose API; they do not
  expose helpers. Header weight is a tax on every translation unit that includes
  them.

---

## Objects are valid at construction (RAII)

The central pattern. If construction succeeds, the object is always valid; using
it afterward needs no `isValid()` / error-code check.

1. **RAII everywhere.** Every object that owns a resource acquires it in the
   constructor and releases it in the destructor. No `init()`/`destroy()` pairs,
   no two-phase construction, no zombie objects.
   ```cpp
   Buffer buf(BufferBuilder(size).storage());   // acquires handle + allocation
   // ~Buffer() frees automatically — no manual cleanup
   ```
2. **Builder passed to constructor.** Configuration is gathered in a builder,
   then passed by reference to the object's constructor. Construction is a single
   expression; the caller chooses storage class (stack, `make_unique`,
   `emplace_back`) freely.
3. **Fluent builders.** Builder methods return `*this`; options are opt-in and
   defaults are safe, so configuration reads like English.
   ```cpp
   BufferBuilder(size).storage().hostVisible().transferDestination()
   ```
4. **Throw in the builder/constructor.** All validation happens before or during
   construction; failure throws with a descriptive message. Post-construction,
   the object is usable unconditionally.
5. **Builders produce guaranteed-valid objects.** A builder that returns an
   empty or half-populated object is worse than no builder. Prefer "factory
   returns valid `T` or throws" over "default-construct then mutate."
6. **Immutable after construction.** Objects do not expose setters.
   Reconfiguration means constructing a new object. No invalid intermediate
   states; trivial to reason about; thread-safe by default.
7. **Configuration and operation are separate.** Builders carry configuration and
   make no resource calls; objects carry state and behavior. Never mixed.

---

## Ownership and lifetime

1. **The library user owns lifetime.** No `shared_ptr`, no reference counting, no
   GC on the hot path. Scope or container membership *is* the lifetime.
   Predictable performance, no hidden overhead, no ownership ambiguity.
2. **Dependencies are constructor parameters, not setters or globals.** If an
   object needs a `Commands` or a `VulkanContext`, it takes one by reference,
   making the dependency and the lifetime relationship visible at the call site.
   Hierarchy is enforced structurally, so lifetime bugs become compile errors or
   immediate throws.
3. **Move-only for resource owners.** Delete copy; define move (transfer
   ownership, null the source). Single ownership enforced by the type system — no
   double-free, no silent shallow copy of a handle.
4. **Deferred destruction can be transparent.** Where a resource cannot be freed
   immediately (in-flight GPU work), collect it and free it when safe, without
   forcing the user to track fences. Correct-by-design safety with no caller
   burden.
5. **Runtime invariant guards where the type system cannot reach.** A static
   guard that throws "multiple frames in flight" turns misuse into an immediate,
   descriptive exception instead of undefined behavior.

---

## Type safety over runtime checks

Prefer templated overloads, `enum class` arguments, friend access, and private
ctors over "throws if wrong, comment says don't." Encode contracts in signatures
and access modifiers; resort to asserts and comments only when the type system
cannot express the rule.

- **Scoped enums replace raw flags.** Wrap vendor integer flags in `enum class`;
  provide explicit `operator|`. Passing the wrong enum is a compile error; the
  underlying integer stays zero-cost.
- **Compile-time validation via CRTP/templates.** `static_assert` a size or shape
  constraint once per instantiation, not at every call site or at a driver crash.
- **Typed overloads retire casts.** Add an `enum class` overload beside an `int`
  one and forward; sed-strip `(int)Enum::X` casts from call sites. Wrong
  enum+target combos then fail to compile.
- **Implicit conversion for interop.** Where a wrapper must drop into a raw API,
  an implicit `operator Handle()` removes `.get()`/`.handle()` noise — used
  deliberately, only for thin owning wrappers.
- **Use standard containers.** `std::vector` / `std::array` / `std::span` are the
  defaults. Hand-rolled fixed arrays, SOA, or custom allocators must justify
  themselves in review with a profile.

---

## Composable objects, not features

A file named for a feature or a verb (`find_replace`, `external_modification`,
`edit_history_integration`, `follow_edits`, `recovery`, `audit`) is a smell: it
names an activity, not a thing that encapsulates the activity. Often the verb
filename is the only smell — the code is already an object, badly named.

1. **Behavior belongs to an object; public free functions do not carry
   behavior.** Every public operation is a member of the object that owns the
   relevant state or concept. Free functions are allowed only as `.cpp`-local
   helpers, never in a public header — even for stateless logic (a pure transform
   becomes a class with verb methods on a value).
2. **Commands are members of the domain object they act on.** Editing commands
   are methods on `Document`; file/workspace commands are methods on `Workspace`.
   A `*_commands` file is not an object — it is a thin registration table binding
   command ids to those methods, containing no business logic. The handler is a
   one-line adapter: look up the target, call its method, map the result.
3. **One primary object per file, named after it; helpers may share the file.**
   Exactly one primary object per file. Builders and value types used only by it
   may live alongside. What a file must not be is a bag of unrelated free
   functions.
4. **Filenames and type names are accurate nouns; `git mv` verb files to their
   object.** Rename the file to the primary object's noun, preserving history.
   If the code is already an object under a verb filename, the fix is just the
   rename.
5. **Objects are valid at construction (reinforces RAII).** When gathering free
   functions into a class, prefer a constructor that takes what the operations
   need. A stateless transform may be default-constructed; a stateful one takes
   its collaborators in the ctor and is usable immediately.

---

## Comments

Make the code say what it can through names, types, and structure. Comment only
for what the code cannot say. Default: delete — a comment that adds nothing lies
as soon as the code around it changes.

**Keep only if it says something the code cannot:**

- Threading / execution contract (UI thread vs audio thread vs callback).
- Magic-number rationale, with units/ranges (`0.15f ~10ms slew at 64 samples`).
- Algorithm reference (`Padé (3,3) tanh`).
- Lifecycle / ordering rule (`attach only while the graph is stopped`).
- Non-local constraint (`consumed by mobile v3; don't reorder fields`).
- Why we hand-rolled something, with a removal condition (`std::atomic isn't
  movable; vector<T> needs the move ctor`).
- Public API contract the signature can't state (idempotency, side effects).

**Delete on sight:**

- Restated field/name (`// the width` above `int width`) → rename instead.
- "Forward declarations", section headings inside a class → use blank lines.
- "`XNode` — does X" preambles → the class name says it.
- "Non-copyable, non-movable" → `= delete` says it.
- History notes → `git log` says it.
- Commented-out (dead) code → delete; VCS remembers.
- A stale comment that disagrees with the code → worse than none; treat as a bug.
- `TODO` with no owner/issue/removal condition, older than a month → fix or delete.

---

## Improving an existing API from consumer evidence

When an API is awkward, start with representative **consumer** code, not the
implementation. Headers show what an API permits; consumers show what it costs to
use correctly.

### Read the consumer as evidence

| Consumer evidence | Likely API problem | Question to ask |
|---|---|---|
| Long comments explaining required call order | Temporal / implicit coupling | Can one operation own the sequence? |
| Several calls that must always occur together | Missing domain operation | What single intent is the caller expressing? |
| Same setup/policy assembly in multiple paths | Missing shared seam | Which part is invariant mechanism vs application policy? |
| Raw ints, IDs, flags, sentinels, parallel arrays | Representation leakage / primitive obsession | Can a type make invalid combinations unrepresentable? |
| Consumers inspect or mutate internals | Broken encapsulation | What query or operation exposes intent instead? |
| Manual waits, invalidation, cleanup, notification | Leaked lifetime / sync protocol | Can RAII or an owning operation make this automatic? |
| Many booleans whose combinations change validity | State-space explosion | Should these be modes, variants, or separate operations? |
| Silent `null`/`0`/empty on misuse | Ambiguous failure | Can the boundary validate and fail loudly? |
| Different names for one concept across layers | Missing shared vocabulary | What domain term should the public API establish? |
| Test setup longer than the behavior under test | API needs too much incidental knowledge | What smaller coherent seam should tests and consumers use? |

### Recover the hidden contract, then place it

1. State the consumer's intended operation in one sentence, without naming
   implementation steps.
2. List the ordering, lifetime, state, sync, and error assumptions currently
   required, and what happens when each is violated.
3. Decide which type should own the contract; describe the smallest public
   operation that enforces it.

The **library** owns: resource validity and lifetime; ordering its own
implementation requires; synchronization/cache invalidation for correctness;
representation invariants; validation of its inputs; coherent shared operations.
The **application** owns: product and presentation policy; user interaction;
workload-specific scheduling and quality tradeoffs; composition of genuinely
independent library operations. When policy must cross the boundary, pass
descriptive configuration or a strategy — the app chooses policy, the library
applies it without the app reproducing internal invariants.

Do not move code into the library merely to shorten a consumer. Move a
responsibility only when it gives the contract one owner, removes duplication,
prevents misuse, or creates a coherent domain operation.

### Attack in leverage order

1. **Capture behavior** — establish an oracle (golden/integration) before
   touching the boundary.
2. **Remove correctness hazards** — invalid states, ambiguous failures, lifetime
   hazards, order-dependent protocols first.
3. **Encapsulate hidden contracts** — replace required call sequences with named
   operations owned by the component that knows why they exist.
4. **Strengthen boundaries** — domain types, scoped enums, validated config,
   actionable errors.
5. **Unify duplicated mechanism** — route consumers through one implementation;
   keep application policy explicit.
6. **Establish vocabulary** — rename around domain intent *after* responsibilities
   are placed.
7. **Remove obsolete comments and glue** — cosmetic cleanup last, so it does not
   hide the underlying coupling.

Correctness and leverage matter more than line count. Prefer tests at the seam
where consumer intent becomes library behavior; unit tests for internal helpers
are useful but do not prove the public boundary is easy to use correctly.

---

## Process — applying this to an existing project

Work bottom-up, in small commits, with a green test suite at every step. Each
step: one identifiable change in the commit subject, builds clean (no new
warnings), passes the existing tests, passes any demo smoke test. Commit each
step alone; if anything regresses, revert that one commit.

Typical order:

1. Move public headers to `include/`; adjust the build, no content changes.
2. Merge tightly-coupled headers the user never uses separately (one header per
   concept, not per format).
3. Move private implementation headers out of `include/` into `src/<area>/`.
4. Delete dead code: unused public types, unused free functions, dead fields.
5. Add RAII constructors that produce valid objects.
6. Move ownership peer-to-peer rather than nested: if A "has-a B" only to
   delegate B's API, make B a peer that registers with A.
7. Convert single-instance `addNode<T>()`-style APIs into named accessors
   (`graph.audioInput()`), so the "only one allowed" check becomes impossible to
   violate.
8. Add type-safe templated overloads beside `int` overloads; sed-strip casts.
9. Rename misleading types and members (`git mv` files to preserve blame). Each
   rename should retire at least one comment.
10. Walk every public header and apply the comment rules — prefer renaming the
    noun over keeping the comment.

---

## Metrics that matter

Targets, tracked before/after a cleanup:

- Public header count — smaller.
- Total public header line count — smaller.
- Comment count in public headers — smaller after applying the rules.
- `(int)` casts at call sites — **zero**.
- Public types with no caller — **zero**.
- Types with two-phase construction — **zero**.
- `x.method()` calls that just delegate to a member — **zero**.
- Public free functions carrying behavior — **zero**.
- `src/*.cpp` whose primary export is free functions, not an object — **zero**.
- Filenames that are verbs/features, not the noun object inside — **zero**.

---

## Worked examples

### vkobjects (Vulkan wrapper — the reference library)

- Public surface is **one umbrella header** (`vkobjects.h` + `rid.h`); every
  public type is PascalCase and RAII.
- Every GPU resource is a move-only RAII wrapper built from a fluent builder:
  `Buffer buf(BufferBuilder(size).storage())`. If it constructs, it is a valid
  `VkBuffer` — and converts implicitly to one for interop.
- Lifetime hierarchy is structural: `VulkanContext` → `Frame` (a runtime guard
  throws on a second live frame) → `Commands` → `Buffer`/`Image`. Deferred
  destruction frees resources once their fence signals, invisibly.
- `Stage`/`Access`/`Layout` scoped enums make barrier misuse a compile error;
  push-constant size is a `static_assert` via CRTP.
- **Pragmatic C-style seam:** `src/vkinternal.h` is a lowercase, non-public
  header of free helpers (`createSampler`, `recordMipmapGeneration`, swapchain
  plumbing) shared across `.cpp`s — objects on top, C-style glue underneath.

### audio library

- `Synth` (which owned the device, all nodes, instruments, sequencers, banks) →
  `AudioGraph`; `SynthNode` → `AudioNode`. The rename deleted the comment "main
  synth class that owns the audio graph."
- `Recorder` (capture + dub + save) split into `OutputCapture` + `DubPlayback`;
  `OutputCapture::arm()` replaced `Recorder::startRecording()`.
- `SampleBank` gained a `SampleBank(path, rootSemitone)` ctor — eighteen lines of
  "create empty, load, populate, push zone" in `main.cpp` became one.
- `addNode<AudioInputNode>()` → `graph.audioInput()`, making the "only one input"
  check impossible to violate. `(int)NodeType::Input::X` at ~900 call sites →
  typed overloads → wrong combos fail to compile.
- Result: 16 public headers → 8; public header comments ~200 → ~40; `(int)` casts
  ~900 → 0; tests 204/204 before and after.

### ssg (this project)

- Repo-wide naming conversion to the case table above; the wire protocol was
  untouched because it serializes by explicit string literals, not C++
  identifiers.
- "Objectify" pass: verb/feature files became composable objects — `hit_test` →
  `HitTester`, `find_replace` → `FindMatcher`/`WorkspaceReplacer`, editing
  commands → stateless `EditInterpreter`/`TextInputInterpreter` transforms over a
  `DocumentSnapshot` value, `*_commands` files reduced to thin registration
  tables.
- Filename pass: class-defining files renamed to their PascalCase primary object
  (`session` → `EditorSession`, `watcher` → `FilesystemWatcher`, `ui_layout` →
  `ShellState`); utility/value-bag/codec/`main`-bearing files kept lowercase.
