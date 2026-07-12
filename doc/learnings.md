# Project learnings

This file records useful project-level tribal knowledge discovered during task
implementation. Children read it before work so they do not repeat expensive
investigations or violate non-obvious constraints.

Record only durable, cross-task knowledge such as:

- surprising platform, compiler, dependency, or build behavior;
- non-obvious API or lifetime constraints;
- reliable debugging or testing techniques specific to SSG;
- integration seams whose practical behavior was not apparent from the specs.

Do not record task summaries, progress, speculation, obvious documentation, or
temporary workarounds. Each entry names the discovering task, states the
learning, and explains its consequence. Promote a learning into
`copilot-instructions.md` or `doc/spec.md` when it becomes a required invariant,
then remove or shorten the redundant entry here.

## foundation-harness

- CMake component manifests are included in the caller's directory scope, but
  `CMAKE_SOURCE_DIR` always names the outermost project's root. SSG captures
  `SSG_SOURCE_DIR` from `CMAKE_CURRENT_SOURCE_DIR` in its root manifest; every
  component manifest must use that variable so `add_subdirectory` consumers do
  not resolve SSG sources beneath the host project.
- `file(GLOB CONFIGURE_DEPENDS)` reliably discovers newly added component
  manifests during builds with Ninja and Makefile generators. Windows parity
  gates that exercise automatic manifest discovery must use Ninja because
  Visual Studio and Xcode generators do not reliably recheck the glob.
- A nested consumer oracle should call an out-of-line public API, not only
  include a header. Linking an out-of-line symbol proves that the component
  source was discovered and compiled from SSG's source root.
