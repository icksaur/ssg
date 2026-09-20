# Developing SSG

SSG is one native terminal-editor application. This guide covers building and
testing it; [`README.md`](README.md) covers installation and use.

## Requirements

- CMake 3.21 or newer
- A C++20 compiler

Linux and native Windows are supported. Windows development requires an x64
MSVC developer command prompt and Ninja.

## Build and test

Configure the development build once, then use the build and fast test loop:

```sh
cmake --preset dev
cmake --build build
ctest --preset dev
```

The `dev` test preset excludes extended recovery and theme tests. Run the full
suite before submitting a change:

```sh
ctest --preset all
```

Release and sanitizer builds use separate directories:

```sh
cmake --preset release && cmake --build build-release
cmake --preset sanitize && cmake --build build-sanitize
ctest --preset sanitize
```

From an x64 MSVC developer command prompt, run the Windows gate with:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

## Platform boundary

Shared application code must not include operating-system headers or call native
APIs. Linux and Windows implementations live under their respective
`src/platform/` directories. `test_file_seam_guard` enforces this boundary, and
platform-specific tests live in the matching `tests/platform/` directory.

Before a Windows release, exercise `ssg.exe` in Windows Terminal and verify text
rendering and Unicode input, keyboard and mouse interaction, resize, desktop
clipboard paste, Git refresh, idle CPU usage, normal quit, and console-close
restoration.

## Project documentation

- `AGENTS.md` — project architecture and development constraints
- `doc/config.md` — compiled configuration guide
- `doc/commands.md` — generated command reference
- `doc/learnings.md` — durable implementation and integration constraints
- `doc/backlog.md` — deferred work
