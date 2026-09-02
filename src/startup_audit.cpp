#include "startup_audit.h"

#include <atomic>

namespace ssg {

namespace {

// Process-wide construction counts, one per OptionalSubsystem.  Atomic so a
// subsystem constructed on a background thread (e.g. a watcher) is still counted
// correctly; relaxed ordering is enough for a diagnostic tally.
std::array<std::atomic<std::uint64_t>, kOptionalSubsystemCount>& ledger() {
    static std::array<std::atomic<std::uint64_t>, kOptionalSubsystemCount>
        counts{};
    return counts;
}

}  // namespace

std::string_view optionalSubsystemName(OptionalSubsystem subsystem) noexcept {
    switch (subsystem) {
        case OptionalSubsystem::Lua: return "lua";
        case OptionalSubsystem::Lsp: return "lsp";
        case OptionalSubsystem::TreeSitterGrammar: return "tree_sitter_grammar";
        case OptionalSubsystem::FilesystemWatcher: return "filesystem_watcher";
        case OptionalSubsystem::Count: break;  // sentinel, never a real subsystem
    }
    return "unknown";
}

void noteOptionalConstruction(OptionalSubsystem subsystem) noexcept {
    ledger()[static_cast<std::size_t>(subsystem)].fetch_add(
        1, std::memory_order_relaxed);
}

std::uint64_t optionalConstructionCount(OptionalSubsystem subsystem) noexcept {
    return ledger()[static_cast<std::size_t>(subsystem)].load(
        std::memory_order_relaxed);
}

std::uint64_t optionalConstructionTotal() noexcept {
    std::uint64_t total = 0;
    for (auto const& count : ledger()) total += count.load(std::memory_order_relaxed);
    return total;
}

void resetOptionalConstructionAudit() noexcept {
    for (auto& count : ledger()) count.store(0, std::memory_order_relaxed);
}

}  // namespace ssg
