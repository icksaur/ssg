#include "WindowsSystemClipboardReader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace ssg {
namespace {

class NativeWindowsClipboard final : public WindowsClipboardApi {
public:
  WindowsClipboardOpen open() override {
    if (::OpenClipboard(nullptr) != 0)
      return WindowsClipboardOpen::Acquired;
    return ::GetLastError() == ERROR_ACCESS_DENIED
               ? WindowsClipboardOpen::Busy
               : WindowsClipboardOpen::Failed;
  }

  void close() noexcept override { (void)::CloseClipboard(); }

  bool unicodeTextAvailable() override {
    return ::IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
  }

  std::optional<std::span<const std::uint8_t>> unicodeTextBytes() override {
    allocation_ = ::GetClipboardData(CF_UNICODETEXT);
    if (allocation_ == nullptr)
      return std::nullopt;
    const SIZE_T size = ::GlobalSize(allocation_);
    data_ = ::GlobalLock(allocation_);
    if (data_ == nullptr)
      return std::nullopt;
    return std::span<const std::uint8_t>{
        static_cast<const std::uint8_t *>(data_), size};
  }

  void releaseUnicodeText() noexcept override {
    if (data_ != nullptr) {
      (void)::GlobalUnlock(allocation_);
      data_ = nullptr;
      allocation_ = nullptr;
    }
  }

  std::chrono::steady_clock::time_point now() override {
    return std::chrono::steady_clock::now();
  }

  void sleepFor(std::chrono::milliseconds duration) override {
    std::this_thread::sleep_for(duration);
  }

private:
  HANDLE allocation_ = nullptr;
  const void *data_ = nullptr;
};

class ClipboardClose {
public:
  explicit ClipboardClose(WindowsClipboardApi &clipboard)
      : clipboard_{clipboard} {}
  ~ClipboardClose() { clipboard_.close(); }

private:
  WindowsClipboardApi &clipboard_;
};

class ClipboardRelease {
public:
  explicit ClipboardRelease(WindowsClipboardApi &clipboard)
      : clipboard_{clipboard} {}
  ~ClipboardRelease() { clipboard_.releaseUnicodeText(); }

private:
  WindowsClipboardApi &clipboard_;
};

SystemClipboardRead convertUnicode(std::span<const std::uint8_t> bytes,
                                   std::size_t byteLimit) {
  if (bytes.size() % sizeof(wchar_t) != 0)
    return {SystemClipboardReadStatus::InvalidUtf8, {}};
  const auto units = bytes.size() / sizeof(wchar_t);
  std::size_t length = 0;
  for (; length < units; ++length) {
    wchar_t unit = 0;
    std::memcpy(&unit, bytes.data() + length * sizeof(wchar_t), sizeof(unit));
    if (unit == L'\0')
      break;
  }
  if (length == units)
    return {SystemClipboardReadStatus::InvalidUtf8, {}};
  if (length == 0)
    return {SystemClipboardReadStatus::Success, {}};
  if (length > byteLimit)
    return {SystemClipboardReadStatus::TooLarge, {}};
  if (length > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return {SystemClipboardReadStatus::TooLarge, {}};

  std::vector<wchar_t> text(length);
  std::memcpy(text.data(), bytes.data(), length * sizeof(wchar_t));
  const int required = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return {::GetLastError() == ERROR_NO_UNICODE_TRANSLATION
                ? SystemClipboardReadStatus::InvalidUtf8
                : SystemClipboardReadStatus::Failed,
            {}};
  }
  if (static_cast<std::size_t>(required) > byteLimit)
    return {SystemClipboardReadStatus::TooLarge, {}};

  std::string utf8(static_cast<std::size_t>(required), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), utf8.data(),
                            required, nullptr, nullptr) != required) {
    return {::GetLastError() == ERROR_NO_UNICODE_TRANSLATION
                ? SystemClipboardReadStatus::InvalidUtf8
                : SystemClipboardReadStatus::Failed,
            {}};
  }
  return {SystemClipboardReadStatus::Success, std::move(utf8)};
}

} // namespace

SystemClipboardRead readWindowsClipboard(WindowsClipboardApi &clipboard,
                                         std::chrono::milliseconds deadline,
                                         std::size_t byteLimit) {
  const auto expires =
      clipboard.now() + std::max(deadline, std::chrono::milliseconds::zero());
  for (;;) {
    switch (clipboard.open()) {
    case WindowsClipboardOpen::Acquired:
      break;
    case WindowsClipboardOpen::Failed:
      return {SystemClipboardReadStatus::Failed, {}};
    case WindowsClipboardOpen::Busy:
      if (clipboard.now() >= expires)
        return {SystemClipboardReadStatus::TimedOut, {}};
      clipboard.sleepFor(std::chrono::milliseconds{1});
      continue;
    }
    break;
  }

  ClipboardClose close{clipboard};
  if (!clipboard.unicodeTextAvailable())
    return {SystemClipboardReadStatus::Unavailable, {}};
  const auto bytes = clipboard.unicodeTextBytes();
  if (!bytes)
    return {SystemClipboardReadStatus::Failed, {}};
  ClipboardRelease release{clipboard};
  return convertUnicode(*bytes, byteLimit);
}

SystemClipboardReader::SystemClipboardReader()
    : SystemClipboardReader{
          {}, kSystemClipboardReadDeadline, kSystemClipboardReadLimit} {}

SystemClipboardReader::SystemClipboardReader(
    std::vector<SystemClipboardProgram>, std::chrono::milliseconds deadline,
    std::size_t byteLimit)
    : deadline_{deadline}, byteLimit_{byteLimit} {}

SystemClipboardRead SystemClipboardReader::read() const {
  NativeWindowsClipboard clipboard;
  return readWindowsClipboard(clipboard, deadline_, byteLimit_);
}

} // namespace ssg
