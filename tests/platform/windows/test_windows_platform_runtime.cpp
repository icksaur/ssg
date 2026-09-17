#include "test_helpers.h"

#include <ssg/PlatformRuntime.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr DWORD controlExitStatus = 0xc000013a;

[[noreturn]] void throwLastError(const char *message) {
  throw std::system_error{static_cast<int>(::GetLastError()),
                          std::system_category(), message};
}

class Console {
public:
  Console() {
    (void)::FreeConsole();
    if (::AllocConsole() == 0) {
      throwLastError("failed to allocate test console");
    }
    input_ = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    output_ = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (input_ == INVALID_HANDLE_VALUE || output_ == INVALID_HANDLE_VALUE ||
        ::SetStdHandle(STD_INPUT_HANDLE, input_) == 0 ||
        ::SetStdHandle(STD_OUTPUT_HANDLE, output_) == 0) {
      throwLastError("failed to initialize test console");
    }
  }

  ~Console() {
    if (input_ != INVALID_HANDLE_VALUE) {
      (void)::CloseHandle(input_);
    }
    if (output_ != INVALID_HANDLE_VALUE) {
      (void)::CloseHandle(output_);
    }
    (void)::FreeConsole();
  }

  Console(const Console &) = delete;
  Console &operator=(const Console &) = delete;

  void write(std::span<const INPUT_RECORD> records) const {
    DWORD written = 0;
    if (::WriteConsoleInputW(input_, records.data(),
                             static_cast<DWORD>(records.size()),
                             &written) == 0 ||
        written != records.size()) {
      throwLastError("failed to write test console input");
    }
  }

private:
  HANDLE input_ = INVALID_HANDLE_VALUE;
  HANDLE output_ = INVALID_HANDLE_VALUE;
};

INPUT_RECORD key(wchar_t text, WORD repeat = 1) {
  INPUT_RECORD record{};
  record.EventType = KEY_EVENT;
  record.Event.KeyEvent.bKeyDown = TRUE;
  record.Event.KeyEvent.wRepeatCount = repeat;
  record.Event.KeyEvent.wVirtualKeyCode = 'A';
  record.Event.KeyEvent.uChar.UnicodeChar = text;
  return record;
}

INPUT_RECORD resize() {
  INPUT_RECORD record{};
  record.EventType = WINDOW_BUFFER_SIZE_EVENT;
  record.Event.WindowBufferSizeEvent.dwSize = {120, 40};
  return record;
}

INPUT_RECORD focus() {
  INPUT_RECORD record{};
  record.EventType = FOCUS_EVENT;
  record.Event.FocusEvent.bSetFocus = TRUE;
  return record;
}

bool woke(const ssg::PlatformReadiness &readiness, std::size_t index) {
  return std::find(readiness.wakes.begin(), readiness.wakes.end(), index) !=
         readiness.wakes.end();
}

TEST(consoleInputAndWakeCanBecomeReadyTogether) {
  Console console;
  ssg::PlatformEventLoop loop{{true, false}};
  ssg::PlatformWake wake;
  const std::array<const ssg::PlatformWake *, 1> wakes{&wake};
  const auto record = key(L'a');
  console.write({&record, 1});
  wake.notify();

  const auto readiness = loop.wait(1s, wakes);
  ASSERT_TRUE(readiness.input);
  ASSERT_TRUE(woke(readiness, 0));
  std::array<char, 8> bytes{};
  const auto count = loop.readInput(bytes);
  ASSERT_EQ(std::string_view(bytes.data(), count), std::string_view{"a"});
  wake.consume();
}

TEST(timeoutAndIgnorableRecordsReturnNoReadiness) {
  Console console;
  ssg::PlatformEventLoop loop{{true, false}};
  const auto record = focus();
  console.write({&record, 1});

  const auto readiness = loop.wait(10ms, {});
  ASSERT_FALSE(readiness.input);
  ASSERT_FALSE(readiness.resize);
  ASSERT_TRUE(readiness.wakes.empty());
}

TEST(indefiniteWaitReturnsTranslatedInputAndResize) {
  Console console;
  ssg::PlatformEventLoop loop{{true, false}};
  std::thread producer{[&] {
    std::this_thread::sleep_for(10ms);
    const std::array records{resize(), key(L'a')};
    console.write(records);
  }};

  const auto readiness = loop.wait(std::nullopt, {});
  producer.join();
  ASSERT_TRUE(readiness.input);
  ASSERT_TRUE(readiness.resize);
}

TEST(translatedInputRemainsReadyUntilFullyRead) {
  Console console;
  ssg::PlatformEventLoop loop{{true, false}};
  const auto record = key(L'x', 3);
  console.write({&record, 1});
  ASSERT_TRUE(loop.wait(1s, {}).input);

  std::array<char, 2> first{};
  ASSERT_EQ(loop.readInput(first), first.size());
  ASSERT_EQ(std::string_view(first.data(), first.size()),
            std::string_view{"xx"});
  ASSERT_TRUE(loop.wait(0ms, {}).input);

  std::array<char, 2> second{};
  const auto count = loop.readInput(second);
  ASSERT_EQ(count, std::size_t{1});
  ASSERT_EQ(std::string_view(second.data(), count), std::string_view{"x"});
}

int runControlChild() {
  ssg::PlatformEventLoop loop{{false, true}};
  if (::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, ::GetCurrentProcessId()) ==
      0) {
    return 10;
  }
  const auto readiness = loop.wait(2s, {});
  if (!readiness.termination ||
      readiness.termination->kind != ssg::TerminationKind::Break) {
    return 11;
  }
  ssg::terminateProcess(*readiness.termination);
}

TEST(consoleControlBecomesTypedTerminationAndPlatformExit) {
  std::array<wchar_t, 32'768> executable{};
  const DWORD length =
      ::GetModuleFileNameW(nullptr, executable.data(), executable.size());
  ASSERT_TRUE(length > 0 && length < executable.size());
  if (length == 0 || length >= executable.size()) {
    return;
  }

  std::wstring command = L"\"" + std::wstring{executable.data(), length} +
                         L"\" --suite test_windows_platform_runtime "
                         L"--control-child";
  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL created = ::CreateProcessW(
      executable.data(), mutableCommand.data(), nullptr, nullptr, FALSE,
      CREATE_NEW_PROCESS_GROUP | CREATE_NEW_CONSOLE, nullptr, nullptr, &startup,
      &process);
  ASSERT_TRUE(created != FALSE);
  if (created == FALSE) {
    return;
  }

  const DWORD waited = ::WaitForSingleObject(process.hProcess, 5'000);
  ASSERT_EQ(waited, WAIT_OBJECT_0);
  if (waited != WAIT_OBJECT_0) {
    (void)::TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
    (void)::WaitForSingleObject(process.hProcess, 1'000);
  }
  DWORD status = 0;
  ASSERT_TRUE(::GetExitCodeProcess(process.hProcess, &status) != FALSE);
  ASSERT_EQ(status, controlExitStatus);
  (void)::CloseHandle(process.hThread);
  (void)::CloseHandle(process.hProcess);
}

TEST(platformIdentityAndClockAreNative) {
  ASSERT_EQ(ssg::processId(),
            static_cast<std::uint64_t>(::GetCurrentProcessId()));
  const auto before = ssg::monotonicTime();
  const auto after = ssg::monotonicTime();
  ASSERT_TRUE(after >= before);
}

} // namespace

SSG_TEST_SUITE_ARGS(test_windows_platform_runtime) {
  if (argc == 2 && std::string_view{argv[1]} == "--control-child") {
    return runControlChild();
  }
  RUN(consoleInputAndWakeCanBecomeReadyTogether);
  RUN(timeoutAndIgnorableRecordsReturnNoReadiness);
  RUN(indefiniteWaitReturnsTranslatedInputAndResize);
  RUN(translatedInputRemainsReadyUntilFullyRead);
  RUN(consoleControlBecomesTypedTerminationAndPlatformExit);
  RUN(platformIdentityAndClockAreNative);
  return failed == 0 ? 0 : 1;
}
