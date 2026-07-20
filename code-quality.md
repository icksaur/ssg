# Code quality

SSG follows `cpp-values.md`.
Cross-cutting implementation invariants are in `copilot-instructions.md`.

Priority order:

1. Correctness
2. Maintainability
3. Simplicity
4. Performance demonstrated by the budgets in `doc/spec.md`

Project rules:

- Prefer less code and one behavior path.
- Make invalid states unrepresentable with strong domain types.
- Use RAII and explicit, caller-owned lifetimes.
- Resource owners are move-only and valid after construction.
- Separate configuration from operational state.
- Keep the editor core independent of transports and renderers.
- Put platform-specific behavior behind adapter boundaries.
- Test public seams with independent oracles before implementation.
- Comments explain contracts and rationale, not code narration.
