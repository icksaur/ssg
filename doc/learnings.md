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

## reference-editor

- Deleting a final line without a trailing newline must also consume its
  preceding newline. Deleting only the final line's byte range leaves an
  unintended empty trailing line.
- Multi-selection transforms that operate on touched lines must partition line
  starts into contiguous runs. Replacing one span from the first touched line
  through the last would modify untouched lines between disjoint selections.

## platform-file-io

- Durable Linux atomic replacement requires flushing the temporary file before
  rename and flushing the parent directory after rename. Flushing only file
  contents does not make the directory-entry replacement crash durable.

## http-cross-platform

- An HTTP upgrade read may contain bytes from the first WebSocket frame after
  the header terminator. The transport must preserve and feed those bytes into
  frame decoding rather than discarding them with the handshake buffer.
- Cross-platform native socket handles require a pointer-width representation
  and an explicit invalid sentinel. Narrowing a Windows `SOCKET` to `int` can
  corrupt valid handles.

## settings-model

- State import or reload must advance mutation generations so compensating
  actions created before the import cannot become valid again through an ABA
  value cycle.
- A feature command set enumerates every command ID normatively owned by that
  feature even when downstream session assembly binds handlers or presentation.
