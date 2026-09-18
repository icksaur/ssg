#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
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

// Translated bytes remain ready until read. Resize is returned only from the
// append call that observes it and is never retained as buffer state.
class WindowsConsoleInputBuffer {
public:
  [[nodiscard]] bool append(std::span<const INPUT_RECORD> records,
                            COORD visibleWindowOrigin);
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] std::size_t read(std::span<char> destination);

private:
  WindowsConsoleInputTranslator translator_;
  std::string bytes_;
};

} // namespace ssg
