#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ssg {

// The surface that currently receives keyboard input.  One value is authoritative
// session state; clients route keys by it.
enum class FocusTarget : std::uint8_t { Editor, Panel, Prompt, ExternalModification };

// The lowercase keymap-context name for a focus target.  A keymap binding's
// context is this name (or "*"); see keymap_contexts().
[[nodiscard]] constexpr std::string_view focusTargetName(
    FocusTarget target) noexcept {
    switch (target) {
    case FocusTarget::Editor:
        return "editor";
    case FocusTarget::Panel:
        return "panel";
    case FocusTarget::Prompt:
        return "prompt";
    case FocusTarget::ExternalModification:
        return "external";
    }
    return {};
}

// The canonical set of valid keymap contexts: the global context "*" plus every
// FocusTarget name.  Derived from FocusTarget so the two cannot drift; a binding
// whose context is outside this set is rejected by validate_keymap
// (KeymapErrorCode::unknown_context).
[[nodiscard]] constexpr std::array<std::string_view, 5>
keymapContexts() noexcept {
    return {"*", focusTargetName(FocusTarget::Editor),
            focusTargetName(FocusTarget::Panel),
            focusTargetName(FocusTarget::Prompt),
            focusTargetName(FocusTarget::ExternalModification)};
}

}  // namespace ssg
