#pragma once

#include <ssg/SystemClipboardReader.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ssg {

enum class WindowsClipboardOpen { Acquired, Busy, Failed };

class WindowsClipboardApi {
public:
  virtual ~WindowsClipboardApi() = default;
  [[nodiscard]] virtual WindowsClipboardOpen open() = 0;
  virtual void close() noexcept = 0;
  [[nodiscard]] virtual bool unicodeTextAvailable() = 0;
  [[nodiscard]] virtual std::optional<std::span<const std::uint8_t>>
  unicodeTextBytes() = 0;
  virtual void releaseUnicodeText() noexcept = 0;
  [[nodiscard]] virtual std::chrono::steady_clock::time_point now() = 0;
  virtual void sleepFor(std::chrono::milliseconds duration) = 0;
};

[[nodiscard]] SystemClipboardRead
readWindowsClipboard(WindowsClipboardApi &clipboard,
                     std::chrono::milliseconds deadline, std::size_t byteLimit);

} // namespace ssg
