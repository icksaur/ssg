#pragma once

#include <ssg/Terminal.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <memory>
#include <span>

namespace ssg {

class WindowsConsoleApi {
public:
  virtual ~WindowsConsoleApi() = default;
  [[nodiscard]] virtual bool inputMode(DWORD &mode) = 0;
  [[nodiscard]] virtual bool outputMode(DWORD &mode) = 0;
  [[nodiscard]] virtual bool setInputMode(DWORD mode) = 0;
  [[nodiscard]] virtual bool setOutputMode(DWORD mode) = 0;
  [[nodiscard]] virtual UINT outputCodePage() = 0;
  [[nodiscard]] virtual bool setOutputCodePage(UINT codePage) = 0;
  [[nodiscard]] virtual bool write(std::span<const char> bytes) = 0;
  [[nodiscard]] virtual bool screenSize(CONSOLE_SCREEN_BUFFER_INFO &info) = 0;
};

[[nodiscard]] std::unique_ptr<NativeTerminal>
makeWindowsTerminal(std::unique_ptr<WindowsConsoleApi> console);

} // namespace ssg
