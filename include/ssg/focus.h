#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ssg {

// The surface that currently receives keyboard input.  One value is authoritative
// session state; clients route keys by it (doc/spec-navigation.md).
enum class FocusTarget : std::uint8_t { Editor, Panel, Prompt };

// The lowercase keymap-context name for a focus target.  A keymap binding's
// context is this name (or "*"); see keymap_contexts() and doc/spec-keymap.md.
[[nodiscard]] constexpr std::string_view focus_target_name(
    FocusTarget target) noexcept {
    switch (target) {
    case FocusTarget::Editor:
        return "editor";
    case FocusTarget::Panel:
        return "panel";
    case FocusTarget::Prompt:
        return "prompt";
    }
    return {};
}

// The canonical set of valid keymap contexts: the global context "*" plus every
// FocusTarget name.  Derived from FocusTarget so the two cannot drift; a binding
// whose context is outside this set is rejected by validate_keymap
// (KeymapErrorCode::unknown_context).
[[nodiscard]] constexpr std::array<std::string_view, 4>
keymap_contexts() noexcept {
    return {"*", focus_target_name(FocusTarget::Editor),
            focus_target_name(FocusTarget::Panel),
            focus_target_name(FocusTarget::Prompt)};
}

}  // namespace ssg
