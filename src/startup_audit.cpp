#include <ssg/startup_audit.h>

#include <atomic>

namespace ssg {

namespace {

// Process-wide construction counts, one per OptionalSubsystem.  Atomic so a
// subsystem constructed on a background thread (e.g. a watcher) is still counted
// correctly; relaxed ordering is enough for a diagnostic tally.
std::array<std::atomic<std::uint64_t>, optional_subsystem_count>& ledger() {
    static std::array<std::atomic<std::uint64_t>, optional_subsystem_count>
        counts{};
    return counts;
}

}  // namespace

std::string_view optional_subsystem_name(OptionalSubsystem subsystem) noexcept {
    switch (subsystem) {
        case OptionalSubsystem::lua: return "lua";
        case OptionalSubsystem::lsp: return "lsp";
        case OptionalSubsystem::tree_sitter_grammar: return "tree_sitter_grammar";
        case OptionalSubsystem::filesystem_watcher: return "filesystem_watcher";
        case OptionalSubsystem::http: return "http";
    }
    return "unknown";
}

void note_optional_construction(OptionalSubsystem subsystem) noexcept {
    ledger()[static_cast<std::size_t>(subsystem)].fetch_add(
        1, std::memory_order_relaxed);
}

std::uint64_t optional_construction_count(OptionalSubsystem subsystem) noexcept {
    return ledger()[static_cast<std::size_t>(subsystem)].load(
        std::memory_order_relaxed);
}

std::uint64_t optional_construction_total() noexcept {
    std::uint64_t total = 0;
    for (auto const& count : ledger()) total += count.load(std::memory_order_relaxed);
    return total;
}

void reset_optional_construction_audit() noexcept {
    for (auto& count : ledger()) count.store(0, std::memory_order_relaxed);
}

}  // namespace ssg
