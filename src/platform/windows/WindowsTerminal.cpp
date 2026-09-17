#include "WindowsTerminal.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <vector>

namespace ssg {
namespace {

class NativeWindowsConsole final : public WindowsConsoleApi {
public:
  NativeWindowsConsole()
      : input_{::GetStdHandle(STD_INPUT_HANDLE)},
        output_{::GetStdHandle(STD_OUTPUT_HANDLE)} {}

  bool inputMode(DWORD &mode) override {
    return valid(input_) && ::GetConsoleMode(input_, &mode) != 0;
  }

  bool outputMode(DWORD &mode) override {
    return valid(output_) && ::GetConsoleMode(output_, &mode) != 0;
  }

  bool setInputMode(DWORD mode) override {
    return valid(input_) && ::SetConsoleMode(input_, mode) != 0;
  }

  bool setOutputMode(DWORD mode) override {
    return valid(output_) && ::SetConsoleMode(output_, mode) != 0;
  }

  UINT outputCodePage() override { return ::GetConsoleOutputCP(); }

  bool setOutputCodePage(UINT codePage) override {
    return ::SetConsoleOutputCP(codePage) != 0;
  }

  bool write(std::span<const char> bytes) override {
    if (bytes.empty())
      return true;
    if (!valid(output_) || bytes.size() > static_cast<std::size_t>(
                                              std::numeric_limits<int>::max()))
      return false;
    const int required =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                              static_cast<int>(bytes.size()), nullptr, 0);
    if (required <= 0)
      return false;
    std::vector<wchar_t> wide(static_cast<std::size_t>(required));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                              static_cast<int>(bytes.size()), wide.data(),
                              required) != required)
      return false;
    std::size_t offset = 0;
    while (offset < wide.size()) {
      DWORD written = 0;
      const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
          wide.size() - offset, static_cast<std::size_t>(MAXDWORD)));
      if (::WriteConsoleW(output_, wide.data() + offset, requested, &written,
                          nullptr) == 0 ||
          written == 0)
        return false;
      offset += written;
    }
    return true;
  }

  bool screenSize(CONSOLE_SCREEN_BUFFER_INFO &info) override {
    return valid(output_) && ::GetConsoleScreenBufferInfo(output_, &info) != 0;
  }

private:
  static bool valid(HANDLE handle) {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
  }

  HANDLE input_;
  HANDLE output_;
};

class WindowsTerminal final : public NativeTerminal {
public:
  explicit WindowsTerminal(std::unique_ptr<WindowsConsoleApi> console)
      : console_{std::move(console)} {}

  ~WindowsTerminal() override { restore(); }

  bool activate() override {
    if (!console_ || !console_->inputMode(originalInputMode_) ||
        !console_->outputMode(originalOutputMode_)) {
      return false;
    }
    originalOutputCodePage_ = console_->outputCodePage();
    if (originalOutputCodePage_ == 0)
      return false;

    DWORD inputMode =
        originalInputMode_ |
        (ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS);
    inputMode &=
        ~(ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
          ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    const DWORD outputMode =
        originalOutputMode_ |
        (ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    if (!console_->setInputMode(inputMode))
      return false;
    inputChanged_ = true;
    if (!console_->setOutputMode(outputMode)) {
      restore();
      return false;
    }
    outputChanged_ = true;
    if (!console_->setOutputCodePage(CP_UTF8)) {
      restore();
      return false;
    }
    codePageChanged_ = true;
    return true;
  }

  void restore() noexcept override {
    if (codePageChanged_) {
      codePageChanged_ = false;
      (void)console_->setOutputCodePage(originalOutputCodePage_);
    }
    if (outputChanged_) {
      outputChanged_ = false;
      (void)console_->setOutputMode(originalOutputMode_);
    }
    if (inputChanged_) {
      inputChanged_ = false;
      (void)console_->setInputMode(originalInputMode_);
    }
  }

  void write(std::string_view bytes) noexcept override {
    (void)console_->write(std::span<const char>{bytes.data(), bytes.size()});
  }

  bool supportsKeyboardProtocol() const noexcept override { return false; }

private:
  std::unique_ptr<WindowsConsoleApi> console_;
  DWORD originalInputMode_ = 0;
  DWORD originalOutputMode_ = 0;
  UINT originalOutputCodePage_ = 0;
  bool inputChanged_ = false;
  bool outputChanged_ = false;
  bool codePageChanged_ = false;
};

} // namespace

std::unique_ptr<NativeTerminal>
makeWindowsTerminal(std::unique_ptr<WindowsConsoleApi> console) {
  return std::make_unique<WindowsTerminal>(std::move(console));
}

std::unique_ptr<NativeTerminal> makePlatformTerminal() {
  return makeWindowsTerminal(std::make_unique<NativeWindowsConsole>());
}

void writeAll(std::string_view bytes) {
  NativeWindowsConsole console;
  (void)console.write(std::span<const char>{bytes.data(), bytes.size()});
}

ViewportDimensions terminalSize() {
  NativeWindowsConsole console;
  CONSOLE_SCREEN_BUFFER_INFO information{};
  if (!console.screenSize(information))
    return {80, 24};
  const int width = information.srWindow.Right - information.srWindow.Left + 1;
  const int height = information.srWindow.Bottom - information.srWindow.Top + 1;
  if (width <= 0 || height <= 0)
    return {80, 24};
  return {static_cast<std::uint32_t>(width),
          static_cast<std::uint32_t>(height)};
}

} // namespace ssg
