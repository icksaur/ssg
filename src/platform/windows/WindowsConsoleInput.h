#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <optional>
#include <span>
#include <string>

namespace ssg {

struct WindowsConsoleInputTranslation {
  std::string bytes;
  bool resize = false;
};

// One translator owns one console input stream. Recreating it between read
// batches loses split surrogate pairs and mouse-button continuity.
class WindowsConsoleInputTranslator {
public:
  [[nodiscard]] WindowsConsoleInputTranslation
  translate(std::span<const INPUT_RECORD> records, COORD visibleWindowOrigin);

private:
  std::optional<KEY_EVENT_RECORD> pendingHighSurrogate_;
  DWORD pressedButtons_ = 0;
};

} // namespace ssg
