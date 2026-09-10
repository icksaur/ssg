#pragma once

#include <ssg/Viewport.h>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <termios.h>
#include <vector>

namespace ssg {

struct SignalEvents {
    bool resize = false;
    std::optional<int> terminate;
};

[[nodiscard]] SignalEvents classifySignalTags(std::string_view drained);

struct TerminalMode {
    std::string_view enter;
    std::string_view leave;
};

inline constexpr TerminalMode kAlternateScreen{"\x1b[?1049h", "\x1b[?1049l"};
inline constexpr TerminalMode kCursorStyleBar{"\x1b[5 q", "\x1b[0 q"};
inline constexpr TerminalMode kMouseButtons{"\x1b[?1000h", "\x1b[?1000l"};
inline constexpr TerminalMode kMouseMotion{"\x1b[?1002h", "\x1b[?1002l"};
inline constexpr TerminalMode kMouseSgrCoordinates{"\x1b[?1006h", "\x1b[?1006l"};
inline constexpr TerminalMode kCursorHidden{"\x1b[?25l", "\x1b[?25h"};
inline constexpr TerminalMode kBracketedPaste{"\x1b[?2004h", "\x1b[?2004l"};

// Kitty keyboard mode is stack-based, so it must only be left by the guard
// paired with its successful entry and must not join kAllModes.
inline constexpr TerminalMode kKeyboardProtocol{"\x1b[>1u", "\x1b[<u"};

inline constexpr TerminalMode kAllModes[]{
    kAlternateScreen, kCursorStyleBar,      kMouseButtons,
    kMouseMotion,     kMouseSgrCoordinates, kCursorHidden,
    kBracketedPaste,
};

class TerminalModes {
  public:
    using Writer = std::function<void(std::string_view)>;

    explicit TerminalModes(Writer writer);
    ~TerminalModes();
    TerminalModes(const TerminalModes&) = delete;
    TerminalModes& operator=(const TerminalModes&) = delete;

    class Guard {
      public:
        Guard() noexcept = default;
        ~Guard();
        Guard(Guard&& other) noexcept;
        Guard& operator=(Guard&& other) noexcept;
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

      private:
        friend class TerminalModes;
        Guard(TerminalModes& owner, std::size_t depth) noexcept
            : owner_{&owner}, depth_{depth} {}
        TerminalModes* owner_ = nullptr;
        std::size_t depth_ = 0;
    };

    [[nodiscard]] Guard enter(TerminalMode mode);
    [[nodiscard]] std::size_t depth() const noexcept;

  private:
    void leaveThrough(std::size_t depth) noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TerminalSession {
  public:
    TerminalSession();
    ~TerminalSession();
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    void restore() noexcept;
    [[nodiscard]] bool active() const noexcept;
    void enableKeyboardProtocol();

  private:
    TerminalModes modes_;
    std::vector<TerminalModes::Guard> entered_;
    termios original_{};
    bool active_ = false;
    bool keyboardProtocolEntered_ = false;
};

void writeAll(std::string_view bytes);
[[nodiscard]] ViewportDimensions terminalSize();

} // namespace ssg
