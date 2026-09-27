Judge design choices by whether a change becomes easier to understand and make
correctly, rather than by compliance with a particular code shape.
[README.md](README.md) describes the product; [plan.md](plan.md) describes the
work and its correctness contracts.

## Structure

main.cpp is the program: the composition of objects and the story of startup,
primary loops, threads, and shutdown. An `App` or `EditorApp` that merely moves
this story elsewhere adds no useful abstraction. Conversely, putting domain
algorithms in main.cpp does not make them composition.

A useful component owns a recognizable responsibility, including the state and
rules needed to carry it out. Internal complexity is acceptable when consumers
can use the result without participating in that complexity. `vkobjects` is
the reference for this quality, not a template for every implementation.

Directories make good boundaries once a responsibility needs collaborating
files. Cross a boundary to use a capability through its public headers, not to
maintain its internals. A component can depend on another component; prefer
one-way dependencies over mutual knowledge or routing everything through main.
Passing specific data or collaborators usually exposes a better boundary than
passing access to the whole program.

Let components emerge from working code. An abstraction earns its place by
owning a contract, hiding a decision, or expressing a coherent operation, not
just by shortening a file or anticipating another caller. Resource ownership
is a legitimate responsibility; components need not all be pure algorithms.
Directory trees and build-system gates are not substitutes for these decisions.

## Readability at the call site

Read the consumer before improving an API. How much unrelated code must the
reader know, and what must they remember to do correctly?

- Calls that must always occur together often reveal a missing operation.
- Manual invalidation, notification, or cleanup can reveal a correctness rule
  that belongs with the state it protects.
- Tests requiring unrelated program setup can reveal an overly broad dependency.
- Repeated access to another component's internals can mean the boundary is wrong.

These are reasons to investigate, not automatic instructions to add a wrapper.
Keep product choices and coordination of independent operations visible in the
caller; hide sequences required by a component's own implementation.

Prefer objects that are valid when constructed, RAII for resources, and visible
ownership and lifetime. Use types to rule out invalid combinations where that
simplifies usage. Where types cannot express a contract, make failure explicit
rather than indistinguishable from a valid empty result. Plain data can remain
plain data; accessors and extra classes do not establish ownership by themselves.

## Evolution

Refactoring affected code is part of feature work, not a separate cleanup phase.
When a feature exposes one of the problems above, look for a small change that
gives the obligation an owner before extending the workaround. Move the relevant
state and its maintenance rules together; moving functions alone may leave the
coupling intact.

Use existing behavior and focused tests to protect the change. Tests at a
component's public boundary are particularly useful evidence that it can be used
without unrelated machinery. Unit tests exercise our logic with controlled
inputs, not OS-emitted events.

Keep the scope relevant to the feature. In the handoff, mention meaningful
ownership or dependency changes and coupling that remains in the affected path.
If the repair needs broader work, make that tradeoff explicit rather than
silently adding another workaround or undertaking an unrelated redesign.

## Style

Types are nouns in capitalized `CamelCase`; functions are verbs, with functions
and variables in lowercase `camelCase`. Filenames usually match their primary
type. Prefer composition over inheritance.

Use descriptive names for work items, phases, milestones, and steps rather than
alphanumeric codes. "Scene milestone" carries its meaning without requiring the
reader to remember what "M3" stands for.

Comments are a smell when they explain what better names or structure could say.
A short responsibility description can help orient the reader. Preserve reasons,
non-obvious constraints, and contracts the code cannot enforce, including why
some tempting code should not be added.

## Delegation

Use capable models for ownership and design decisions. Put task-specific steps,
scope, contracts, and acceptance criteria in the delegated prompt, especially
when a cheaper model is implementing an already-decided change.

## Vendoring contract

Vendored sources are byte-identical copies, never referenced by sibling path.
`../check-vendored.sh` verifies them. Do not edit a vendored copy; change the
source repository and re-vendor. Single-file libraries (vecmath, tlv, halfedge)
are matched by file name; multi-file libraries (vkobjects) live under
`vendor/<lib>/` and are matched by that relative path.