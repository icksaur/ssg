#include "test_helpers.h"

#include "WindowsSystemClipboardReader.h"

#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::vector<std::uint8_t> wideBytes(std::wstring_view text,
                                    bool terminated = true) {
  std::vector<std::uint8_t> bytes((text.size() + (terminated ? 1 : 0)) *
                                  sizeof(wchar_t));
  if (!text.empty()) {
    std::memcpy(bytes.data(), text.data(), text.size() * sizeof(wchar_t));
  }
  return bytes;
}

class FakeClipboard final : public ssg::WindowsClipboardApi {
public:
  ssg::WindowsClipboardOpen open() override {
    ++openCalls;
    if (busyAttempts-- > 0)
      return ssg::WindowsClipboardOpen::Busy;
    return openResult;
  }
  void close() noexcept override { ++closes; }
  bool unicodeTextAvailable() override { return available; }
  std::optional<std::span<const std::uint8_t>> unicodeTextBytes() override {
    return lockSucceeds ? std::optional<std::span<const std::uint8_t>>{bytes}
                        : std::nullopt;
  }
  void releaseUnicodeText() noexcept override { ++releases; }
  std::chrono::steady_clock::time_point now() override { return nowValue; }
  void sleepFor(std::chrono::milliseconds duration) override {
    nowValue += duration;
  }

  ssg::WindowsClipboardOpen openResult = ssg::WindowsClipboardOpen::Acquired;
  int busyAttempts = 0;
  int openCalls = 0;
  int closes = 0;
  int releases = 0;
  bool available = true;
  bool lockSucceeds = true;
  std::vector<std::uint8_t> bytes = wideBytes(L"");
  std::chrono::steady_clock::time_point nowValue{};
};

TEST(readsEmptyAndMultilineUnicodeText) {
  FakeClipboard clipboard;
  auto read = ssg::readWindowsClipboard(clipboard, 10ms, 1024);
  ASSERT_TRUE(read.accepted());
  ASSERT_TRUE(read.text.empty());
  ASSERT_EQ(clipboard.closes, 1);
  ASSERT_EQ(clipboard.releases, 1);

  clipboard.bytes = wideBytes(L"one\n\u03bb\U0001f600");
  read = ssg::readWindowsClipboard(clipboard, 10ms, 1024);
  ASSERT_TRUE(read.accepted());
  ASSERT_EQ(read.text, std::string{"one\n\xce\xbb\xf0\x9f\x98\x80"});
  ASSERT_EQ(clipboard.closes, 2);
  ASSERT_EQ(clipboard.releases, 2);
}

TEST(rejectsMissingTerminatorMalformedUtf16AndOversize) {
  FakeClipboard clipboard;
  clipboard.bytes = wideBytes(L"text", false);
  ASSERT_EQ(ssg::readWindowsClipboard(clipboard, 10ms, 1024).status,
            ssg::SystemClipboardReadStatus::InvalidUtf8);

  clipboard.bytes = wideBytes(std::wstring_view{L"\xd800", 1});
  ASSERT_EQ(ssg::readWindowsClipboard(clipboard, 10ms, 1024).status,
            ssg::SystemClipboardReadStatus::InvalidUtf8);

  clipboard.bytes = wideBytes(L"12345");
  ASSERT_EQ(ssg::readWindowsClipboard(clipboard, 10ms, 4).status,
            ssg::SystemClipboardReadStatus::TooLarge);
  ASSERT_EQ(clipboard.closes, 3);
  ASSERT_EQ(clipboard.releases, 3);
}

TEST(contentionHonorsDeadlineAndAcquiredPathsAlwaysClose) {
  FakeClipboard clipboard;
  clipboard.busyAttempts = 2;
  clipboard.bytes = wideBytes(L"ready");
  ASSERT_TRUE(ssg::readWindowsClipboard(clipboard, 10ms, 1024).accepted());
  ASSERT_EQ(clipboard.openCalls, 3);
  ASSERT_EQ(clipboard.closes, 1);

  FakeClipboard stalled;
  stalled.busyAttempts = 100;
  ASSERT_EQ(ssg::readWindowsClipboard(stalled, 2ms, 1024).status,
            ssg::SystemClipboardReadStatus::TimedOut);
  ASSERT_EQ(stalled.closes, 0);

  FakeClipboard absent;
  absent.available = false;
  ASSERT_EQ(ssg::readWindowsClipboard(absent, 10ms, 1024).status,
            ssg::SystemClipboardReadStatus::Unavailable);
  ASSERT_EQ(absent.closes, 1);
  ASSERT_EQ(absent.releases, 0);

  FakeClipboard failedLock;
  failedLock.lockSucceeds = false;
  ASSERT_EQ(ssg::readWindowsClipboard(failedLock, 10ms, 1024).status,
            ssg::SystemClipboardReadStatus::Failed);
  ASSERT_EQ(failedLock.closes, 1);
  ASSERT_EQ(failedLock.releases, 0);
}

} // namespace

SSG_TEST_SUITE(test_windows_system_clipboard) {
  RUN(readsEmptyAndMultilineUnicodeText);
  RUN(rejectsMissingTerminatorMalformedUtf16AndOversize);
  RUN(contentionHonorsDeadlineAndAcquiredPathsAlwaysClose);
  return failed == 0 ? 0 : 1;
}
