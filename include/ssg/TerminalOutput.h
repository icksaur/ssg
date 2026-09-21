#pragma once

#include <ssg/ClipboardRegister.h>
#include <ssg/Renderer.h>
#include <ssg/color.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ssg {

[[nodiscard]] std::string encodeClipboardWrite(std::string_view text);

class SystemClipboardWriter {
  public:
    [[nodiscard]] std::optional<std::string> bytesFor(
        const std::optional<ClipboardWrite>& write, bool terminalCanWrite);

  private:
    std::optional<std::uint64_t> served_;
};

[[nodiscard]] std::string encodeFrame(const CellGrid& screen, ColorDepth depth,
                                      bool showCursor = true);
[[nodiscard]] std::string
encodeAnsiFrame(const CellGrid& screen,
                ColorDepth depth = ColorDepth::Truecolor);
[[nodiscard]] ColorDepth detectColorDepth(const char* colorDepthOverride,
                                          const char* colorterm,
                                          const char* term,
                                          const char* termProgram,
                                          const char* windowsTerminalSession =
                                              nullptr);
} // namespace ssg
