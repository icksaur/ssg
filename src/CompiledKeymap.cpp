#include <ssg/CompiledKeymap.h>

#include <algorithm>

namespace ssg {

namespace {

// The authored context name, compiled.  "*" is the global context and every
// other valid name is a focus; a name outside that closed set matches no
// keystroke, exactly as KeymapMatcher's name comparison does.
CompiledContext compileContext(std::string_view name) {
    if (name == "*") return CompiledContext::any();
    for (auto const focus :
         {FocusTarget::Editor, FocusTarget::Panel, FocusTarget::Prompt,
          FocusTarget::ExternalModification}) {
        if (focusTargetName(focus) == name) return CompiledContext::of(focus);
    }
    return CompiledContext::never();
}

}  // namespace

CompiledKeymap::CompiledKeymap(KeymapViewState const& keymap) {
    entries_.reserve(keymap.bindings.size());
    for (auto const& binding : keymap.bindings) {
        Entry entry;
        entry.commandId = binding.commandId;
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
        }
    }
    if (match != nullptr) {
        return {KeymapMatchKind::Resolved, match->commandId};
    }
    return {KeymapMatchKind::None, {}};
}

}  // namespace ssg
