#include "WindowsConsoleInput.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace ssg {
namespace {

constexpr bool highSurrogate(std::uint16_t value) {
  return value >= 0xd800 && value <= 0xdbff;
}

constexpr bool lowSurrogate(std::uint16_t value) {
  return value >= 0xdc00 && value <= 0xdfff;
}

void appendUtf8(std::string &output, std::uint32_t codepoint) {
  if (codepoint <= 0x7f) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

bool control(DWORD state) {
  return (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
}

bool alt(DWORD state) {
  return (state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
}

bool altGr(DWORD state) {
  return (state & RIGHT_ALT_PRESSED) != 0 && control(state);
}

int modifierParameter(DWORD state) {
  const int bits = ((state & SHIFT_PRESSED) != 0 ? 1 : 0) |
                   (alt(state) ? 2 : 0) | (control(state) ? 4 : 0);
  return 1 + bits;
}

std::optional<int> kittyCodepoint(WORD virtualKey) {
  if (virtualKey >= 'A' && virtualKey <= 'Z') {
    return 'a' + static_cast<int>(virtualKey - 'A');
  }
  if (virtualKey >= '0' && virtualKey <= '9') {
    return static_cast<int>(virtualKey);
  }
  switch (virtualKey) {
  case VK_ESCAPE:
    return 27;
  case VK_RETURN:
    return 13;
  case VK_TAB:
    return 9;
  case VK_BACK:
    return 127;
  case VK_SPACE:
    return ' ';
  case VK_OEM_4:
    return '[';
  case VK_OEM_6:
    return ']';
  case VK_OEM_5:
    return '\\';
  case VK_OEM_1:
    return ';';
  case VK_OEM_7:
    return '\'';
  case VK_OEM_COMMA:
    return ',';
  case VK_OEM_PERIOD:
    return '.';
  case VK_OEM_2:
    return '/';
  case VK_OEM_MINUS:
    return '-';
  case VK_OEM_PLUS:
    return '=';
  case VK_OEM_3:
    return '`';
  default:
    return std::nullopt;
  }
}

void appendKitty(std::string &output, int codepoint, DWORD state) {
  output += "\x1b[";
  output += std::to_string(codepoint);
  const int modifier = modifierParameter(state);
  if (modifier != 1) {
    output += ';';
    output += std::to_string(modifier);
  }
  output += 'u';
}

bool appendFunctional(std::string &output, WORD virtualKey, DWORD state) {
  std::string_view plain;
  char final = '\0';
  int tilde = 0;
  switch (virtualKey) {
  case VK_UP:
    plain = "\x1b[A";
    final = 'A';
    break;
  case VK_DOWN:
    plain = "\x1b[B";
    final = 'B';
    break;
  case VK_RIGHT:
    plain = "\x1b[C";
    final = 'C';
    break;
  case VK_LEFT:
    plain = "\x1b[D";
    final = 'D';
    break;
  case VK_HOME:
    plain = "\x1b[H";
    final = 'H';
    break;
  case VK_END:
    plain = "\x1b[F";
    final = 'F';
    break;
  case VK_DELETE:
    plain = "\x1b[3~";
    tilde = 3;
    break;
  case VK_PRIOR:
    plain = "\x1b[5~";
    tilde = 5;
    break;
  case VK_NEXT:
    plain = "\x1b[6~";
    tilde = 6;
    break;
  default:
    return false;
  }
  const int modifier = modifierParameter(state);
  if (modifier == 1) {
    output.append(plain);
  } else if (tilde != 0) {
    output += "\x1b[";
    output += std::to_string(tilde);
    output += ';';
    output += std::to_string(modifier);
    output += '~';
  } else {
    output += "\x1b[1;";
    output += std::to_string(modifier);
    output += final;
  }
  return true;
}

bool appendNamed(std::string &output, WORD virtualKey, DWORD state) {
  if (virtualKey == VK_RETURN) {
    output.push_back('\r');
    return true;
  }
  if (modifierParameter(state) != 1)
    return false;
  switch (virtualKey) {
  case VK_ESCAPE:
    output.push_back('\x1b');
    return true;
  case VK_TAB:
    output.push_back('\t');
    return true;
  case VK_BACK:
    output.push_back('\x7f');
    return true;
  default:
    return false;
  }
}

void appendRepeated(std::string &output, std::string_view bytes, WORD repeat) {
  for (WORD index = 0; index < repeat; ++index)
    output.append(bytes);
}

void appendKey(std::string &output, const KEY_EVENT_RECORD &key,
               std::uint32_t codepoint, WORD repeat) {
  if (codepoint >= 0x20 &&
      ((!control(key.dwControlKeyState) && !alt(key.dwControlKeyState)) ||
       altGr(key.dwControlKeyState))) {
    std::string encoded;
    appendUtf8(encoded, codepoint);
    appendRepeated(output, encoded, repeat);
    return;
  }

  std::string encoded;
  if (!appendNamed(encoded, key.wVirtualKeyCode, key.dwControlKeyState) &&
      !appendFunctional(encoded, key.wVirtualKeyCode, key.dwControlKeyState)) {
    if (auto kitty = kittyCodepoint(key.wVirtualKeyCode)) {
      appendKitty(encoded, *kitty, key.dwControlKeyState);
    } else if (codepoint >= 0x20 && alt(key.dwControlKeyState) &&
               !control(key.dwControlKeyState)) {
      encoded.push_back('\x1b');
      appendUtf8(encoded, codepoint);
    } else {
      return;
    }
  }
  appendRepeated(output, encoded, repeat);
}

int relativeCoordinate(SHORT value, SHORT origin) {
  return std::max(0, static_cast<int>(value) - static_cast<int>(origin));
}

int mouseButton(DWORD state) {
  if ((state & FROM_LEFT_1ST_BUTTON_PRESSED) != 0)
    return 0;
  if ((state & FROM_LEFT_2ND_BUTTON_PRESSED) != 0)
    return 1;
  if ((state & RIGHTMOST_BUTTON_PRESSED) != 0)
    return 2;
  return 3;
}

constexpr DWORD mouseButtons =
    FROM_LEFT_1ST_BUTTON_PRESSED | FROM_LEFT_2ND_BUTTON_PRESSED |
    RIGHTMOST_BUTTON_PRESSED | FROM_LEFT_3RD_BUTTON_PRESSED |
    FROM_LEFT_4TH_BUTTON_PRESSED;

void appendMouse(std::string &output, int code, int column, int row,
                 char final) {
  output += "\x1b[<";
  output += std::to_string(code);
  output += ';';
  output += std::to_string(column + 1);
  output += ';';
  output += std::to_string(row + 1);
  output += final;
}

} // namespace

WindowsConsoleInputTranslation
WindowsConsoleInputTranslator::translate(std::span<const INPUT_RECORD> records,
                                         COORD visibleWindowOrigin) {
  WindowsConsoleInputTranslation result;
  for (const auto &record : records) {
    if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
      result.resize = true;
      continue;
    }
    if (record.EventType == MOUSE_EVENT) {
      const auto &mouse = record.Event.MouseEvent;
      const int column =
          relativeCoordinate(mouse.dwMousePosition.X, visibleWindowOrigin.X);
      const int row =
          relativeCoordinate(mouse.dwMousePosition.Y, visibleWindowOrigin.Y);
      const bool mouseAlt = alt(mouse.dwControlKeyState);
      const DWORD currentButtons = mouse.dwButtonState & mouseButtons;
      const DWORD released = pressedButtons_ & ~currentButtons;
      const DWORD pressed = currentButtons & ~pressedButtons_;
      if (released != 0) {
        appendMouse(result.bytes, mouseButton(released) + (mouseAlt ? 8 : 0),
                    column, row, 'm');
      }
      if (pressed != 0) {
        appendMouse(result.bytes, mouseButton(pressed) + (mouseAlt ? 8 : 0),
                    column, row, 'M');
      }
      if (mouse.dwEventFlags == MOUSE_WHEELED) {
        const auto delta = static_cast<SHORT>(HIWORD(mouse.dwButtonState));
        if (delta != 0) {
          appendMouse(result.bytes, (delta > 0 ? 64 : 65) + (mouseAlt ? 8 : 0),
                      column, row, 'M');
        }
      } else if (mouse.dwEventFlags == MOUSE_MOVED && currentButtons != 0) {
        appendMouse(result.bytes,
                    32 + mouseButton(currentButtons) + (mouseAlt ? 8 : 0),
                    column, row, 'M');
      } else if (mouse.dwEventFlags == DOUBLE_CLICK && currentButtons != 0 &&
                 pressed == 0) {
        appendMouse(result.bytes,
                    mouseButton(currentButtons) + (mouseAlt ? 8 : 0), column,
                    row, 'M');
      }
      pressedButtons_ = currentButtons;
      continue;
    }
    if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
      continue;
    }

    const auto &key = record.Event.KeyEvent;
    const auto unit = static_cast<std::uint16_t>(key.uChar.UnicodeChar);
    if (pendingHighSurrogate_) {
      const auto high =
          static_cast<std::uint16_t>(pendingHighSurrogate_->uChar.UnicodeChar);
      if (lowSurrogate(unit)) {
        const auto codepoint =
            0x10000u + ((static_cast<std::uint32_t>(high) - 0xd800u) << 10) +
            (static_cast<std::uint32_t>(unit) - 0xdc00u);
        const WORD repeat =
            std::min(std::max<WORD>(pendingHighSurrogate_->wRepeatCount, 1),
                     std::max<WORD>(key.wRepeatCount, 1));
        appendKey(result.bytes, key, codepoint, repeat);
        pendingHighSurrogate_.reset();
        continue;
      }
      pendingHighSurrogate_.reset();
    }
    if (highSurrogate(unit)) {
      pendingHighSurrogate_ = key;
      continue;
    }
    if (lowSurrogate(unit))
      continue;
    appendKey(result.bytes, key, unit, std::max<WORD>(key.wRepeatCount, 1));
  }
  return result;
}

} // namespace ssg
