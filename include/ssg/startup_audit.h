#pragma once

// M10-2 optional-subsystem construction audit (doc/spec-fast-startup.md).
//
// Project invariant I12: constructing and using the basic in-process editor —
// and producing the first frame — must construct NO optional subsystem (Lua,
// LSP, a real Tree-sitter grammar, a filesystem watcher, or the HTTP/WebSocket
// server).  Each optional subsystem notes its own construction here; the startup
// oracle drives the first-frame path and asserts the ledger stayed empty, and
// checks the ledger is empty at process entry (no self-registering static init).
//
// This is a process-wide diagnostic ledger, not editor state; it exists so the
// negative ("nothing optional was built") is executable rather than assumed.

#include <array>
#include <cstdint>
#include <string_view>

namespace ssg {

enum class OptionalSubsystem : std::uint8_t {
    lua,
    lsp,
    tree_sitter_grammar,
    filesystem_watcher,
    http,
};

inline constexpr std::size_t optional_subsystem_count = 5;
inline constexpr std::array<OptionalSubsystem, optional_subsystem_count>
    all_optional_subsystems{
        OptionalSubsystem::lua,
        OptionalSubsystem::lsp,
        OptionalSubsystem::tree_sitter_grammar,
        OptionalSubsystem::filesystem_watcher,
        OptionalSubsystem::http,
    };
static_assert(all_optional_subsystems.size() == optional_subsystem_count,
              "every OptionalSubsystem must appear in all_optional_subsystems so "
              "the audit is exhaustive");

[[nodiscard]] std::string_view optional_subsystem_name(OptionalSubsystem) noexcept;

// Record that an optional subsystem has just constructed its heavy resource.
void note_optional_construction(OptionalSubsystem subsystem) noexcept;

// How many times a subsystem (or all subsystems) has been constructed since the
// last reset / process start.
[[nodiscard]] std::uint64_t optional_construction_count(
    OptionalSubsystem subsystem) noexcept;
[[nodiscard]] std::uint64_t optional_construction_total() noexcept;

// Zero the ledger (for a test measuring one first-frame path in isolation).
void reset_optional_construction_audit() noexcept;

}  // namespace ssg
