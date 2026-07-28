#include <ssg/CompiledKeymap.h>

#include <ssg/CommandCatalog.h>

#include <algorithm>

namespace ssg {

namespace {

bool isStrictPrefix(std::span<CompiledStroke const> shorter,
                    CompiledSequence const& longer) {
    return shorter.size() < longer.size() &&
           std::equal(shorter.begin(), shorter.end(), longer.begin());
}

// The authored context name, compiled.  "*" is the global context and every
// other valid name is a focus; a name outside that closed set matches no
// keystroke, exactly as KeymapMatcher's name comparison does.
CompiledContext compileContext(std::string_view name) {
    if (name == "*") return CompiledContext::any();
    for (auto const focus :
         {FocusTarget::Editor, FocusTarget::Panel, FocusTarget::Prompt}) {
        if (focusTargetName(focus) == name) return CompiledContext::of(focus);
    }
    return CompiledContext::never();
}

}  // namespace

CompiledKeymap::CompiledKeymap(KeymapViewState const& keymap,
                               CommandCatalog const& catalog) {
    entries_.reserve(keymap.bindings.size());
    for (auto const& binding : keymap.bindings) {
        Entry entry;
        // Resolved once, here.  A binding may name a command the catalog does
        // not have -- keymap.bind accepts any non-empty id -- and that name is
        // what the resulting rejection must report, so the ref keeps it either
        // way.
        entry.command = CommandRef{binding.commandId,
                                   catalog.handleFor(binding.commandId)};
        entry.context = compileContext(binding.context);
        entry.sequence.reserve(binding.sequence.size());
        for (auto const& stroke : binding.sequence) {
            entry.sequence.emplace_back(stroke);
        }
        entries_.push_back(std::move(entry));
    }
}

CompiledResolution CompiledKeymap::resolve(
    std::span<CompiledStroke const> pending, FocusTarget focus) const {
    if (pending.empty()) return {};

    Entry const* match = nullptr;
    bool hasPending = false;
    for (auto const& entry : entries_) {
        if (!entry.context.eligibleIn(focus)) continue;
        if (std::ranges::equal(entry.sequence, pending)) {
            // First eligible match wins; a global binding upgrades a focus
            // match (K4 precedence) but two global bindings keep the first,
            // matching KeymapMatcher's first-authoritative rule so the two
            // agree on any (even invalid) keymap.
            if (match == nullptr) {
                match = &entry;
            } else if (entry.context.isAny() && !match->context.isAny()) {
                match = &entry;
            }
        } else if (isStrictPrefix(pending, entry.sequence)) {
            hasPending = true;
        }
    }
    if (match != nullptr) {
        return {KeymapMatchKind::Resolved, match->command};
    }
    return {hasPending ? KeymapMatchKind::Pending : KeymapMatchKind::None, {}};
}

}  // namespace ssg
