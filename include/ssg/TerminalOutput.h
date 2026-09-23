#pragma once

#include <ssg/ClipboardRegister.h>
#include <ssg/Renderer.h>
#include <ssg/color.h>

#include <cstdint>
#include <optional>
#include <cstddef>
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

struct RetainedTerminalFrame {
    std::string bytes;
    std::size_t changedCells = 0;
    bool complete = false;
};

class RetainedTerminalEncoder {
  public:
    explicit RetainedTerminalEncoder(ColorDepth depth) : depth_{depth} {}

    // Encoding is speculative until commit(): retained state always describes
    // the last frame handed to the terminal writer.
    [[nodiscard]] RetainedTerminalFrame encode(const CellGrid& screen,
                                               bool showCursor = true) const;
    void commit(const CellGrid& screen, bool showCursor = true);
    void invalidate() noexcept;

  private:
    ColorDepth depth_;
    std::optional<CellGrid> retained_;
    bool retainedCursorVisible_ = true;
    bool invalidated_ = false;
};

[[nodiscard]] ColorDepth detectColorDepth(const char* colorDepthOverride,
                                          const char* colorterm,
                                          const char* term,
                                          const char* termProgram,
                                          const char* windowsTerminalSession =
                                              nullptr,
                                          bool terminalSupportsTruecolor =
                                              false);
} // namespace ssg
