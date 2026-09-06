#pragma once

#include <ssg/Keymap.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ssg {

[[nodiscard]] inline bool
applicationQuitRequested(const KeyStroke& stroke) noexcept {
    return stroke.code == KeyCode::KeyQ && stroke.alt && !stroke.control;
}

enum class DecodeStatus : std::uint8_t {
    none,
    incomplete,
    key,
    scroll,
    pointer,
    paste,
    reply,
};

inline constexpr std::size_t kMaxSequenceBytes = 256;
inline constexpr std::size_t kMaxPasteBytes = 4u * 1024u * 1024u;

enum class PointerButton : std::uint8_t { left, middle, right, other };
enum class PointerKind : std::uint8_t { press, release, drag };

struct PointerEvent {
    int column = 0;
    int row = 0;
    PointerButton button = PointerButton::left;
    PointerKind kind = PointerKind::press;
    bool alt = false;

    friend bool operator==(const PointerEvent&, const PointerEvent&) = default;
};

struct Decoded {
    DecodeStatus status = DecodeStatus::none;
    KeyStroke stroke;
    std::string text;
    std::int64_t scroll = 0;
    PointerEvent pointer;
    std::string reply;
};

[[nodiscard]] Decoded decodeInput(std::string_view bytes, bool inputExhausted,
                                  std::size_t& consumed);

} // namespace ssg
