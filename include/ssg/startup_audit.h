#pragma once

// M10-2 optional-subsystem construction audit.
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
    Lua,
    Lsp,
    TreeSitterGrammar,
    FilesystemWatcher,
    Http,
    Count,  // sentinel; keep last. Ties optional_subsystem_count to the enum.
};

inline constexpr std::size_t kOptionalSubsystemCount =
    static_cast<std::size_t>(OptionalSubsystem::Count);

// Independently sized (deduced from its initializers), then checked against the
// enum-derived count: adding an OptionalSubsystem without listing it here fails
// the static_assert, so the audit stays exhaustive.
inline constexpr auto kAllOptionalSubsystems = std::to_array({
    OptionalSubsystem::Lua,
    OptionalSubsystem::Lsp,
    OptionalSubsystem::TreeSitterGrammar,
    OptionalSubsystem::FilesystemWatcher,
    OptionalSubsystem::Http,
});
static_assert(kAllOptionalSubsystems.size() == kOptionalSubsystemCount,
              "every OptionalSubsystem (before the count_ sentinel) must appear "
              "in all_optional_subsystems so the audit is exhaustive");

[[nodiscard]] std::string_view optionalSubsystemName(OptionalSubsystem) noexcept;

// Record that an optional subsystem has just constructed its heavy resource.
void noteOptionalConstruction(OptionalSubsystem subsystem) noexcept;

// How many times a subsystem (or all subsystems) has been constructed since the
// last reset / process start.
[[nodiscard]] std::uint64_t optionalConstructionCount(
    OptionalSubsystem subsystem) noexcept;
[[nodiscard]] std::uint64_t optionalConstructionTotal() noexcept;

// Zero the ledger (for a test measuring one first-frame path in isolation).
void resetOptionalConstructionAudit() noexcept;

}  // namespace ssg
