# Code quality

SSG follows `cpp-values.md`.
Cross-cutting implementation invariants are in `AGENTS.md`.

Priority order:

1. Correctness
2. Maintainability
3. Simplicity
4. Performance demonstrated by the budgets in the benchmark suite

Project rules:

- Prefer less code and one behavior path.
- Make invalid states unrepresentable with strong domain types.
- Use RAII and explicit, caller-owned lifetimes.
- Resource owners are move-only and valid after construction.
- Separate configuration from operational state.
- Keep the editor core independent of transports and renderers.
- Put platform-specific behavior behind adapter boundaries.
- Test each behavior once at the narrowest stable seam that owns it. Reserve
  independent oracles for answers knowable independently of the implementation.
- Facts live in one place; delete a duplicate list rather than syncing it.
- Comments explain contracts and rationale, not code narration.
