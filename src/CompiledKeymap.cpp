#include <ssg/CompiledKeymap.h>

#include <algorithm>

namespace ssg {

namespace {

bool isStrictPrefix(std::span<CompiledStroke const> shorter,
                    CompiledSequence const& longer) {
    return shorter.size() < longer.size() &&
           std::equal(shorter.begin(), shorter.end(), longer.begin());
}

}  // namespace

CompiledKeymap::CompiledKeymap(KeymapViewState const& keymap) {
    contexts_.emplace("*", kAnyContext);
    auto internContext = [this](std::string const& name) {
        auto const next = static_cast<ContextId>(contexts_.size() + 1);
        return contexts_.emplace(name, next).first->second;
    };
    auto internCode = [](KeyStroke const& stroke) {
        return CompiledStroke{stroke};
    };

    entries_.reserve(keymap.bindings.size());
    for (auto const& binding : keymap.bindings) {
        Entry entry;
        entry.command = commandHandle(binding.commandId);
        entry.context = internContext(binding.context);
        entry.sequence.reserve(binding.sequence.size());
        for (auto const& stroke : binding.sequence) {
            entry.sequence.push_back(internCode(stroke));
        }
        entries_.push_back(std::move(entry));
    }
}

ContextId CompiledKeymap::contextFor(std::string_view name) const {
    auto const found = contexts_.find(std::string{name});
    return found == contexts_.end() ? kUnknownContext : found->second;
}

CompiledResolution CompiledKeymap::resolve(
    std::span<CompiledStroke const> pending, ContextId context) const {
    if (pending.empty()) return {};

    Entry const* match = nullptr;
    bool hasPending = false;
    for (auto const& entry : entries_) {
        if (entry.context != kAnyContext && entry.context != context) {
            continue;
        }
        if (std::ranges::equal(entry.sequence, pending)) {
            if (match == nullptr) {
                match = &entry;
            } else if (entry.context == kAnyContext &&
                       match->context != kAnyContext) {
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
