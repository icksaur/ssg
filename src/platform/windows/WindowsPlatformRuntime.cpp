#include <ssg/PlatformRuntime.h>

#include "WindowsConsoleInput.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ssg {
namespace {

constexpr DWORD noControlRequest = std::numeric_limits<DWORD>::max();
constexpr DWORD cleanupWaitMilliseconds = 5'000;
constexpr DWORD controlExitStatus = 0xc000013a;

std::atomic<HANDLE> activeControlEvent{};
std::atomic<HANDLE> activeCleanupEvent{};
std::atomic<DWORD> activeControlRequest{noControlRequest};

[[noreturn]] void throwLastError(const char *message) {
  throw std::system_error{static_cast<int>(::GetLastError()),
                          std::system_category(), message};
}

struct ProcessControlEvents {
  ProcessControlEvents()
      : control{::CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        cleanup{::CreateEventW(nullptr, TRUE, FALSE, nullptr)} {
    if (control != nullptr && cleanup != nullptr)
      return;
    const int error = static_cast<int>(::GetLastError());
    if (control != nullptr)
      (void)::CloseHandle(control);
    if (cleanup != nullptr)
      (void)::CloseHandle(cleanup);
    throw std::system_error{error, std::system_category(),
                            "failed to create console control events"};
  }

  HANDLE control;
  HANDLE cleanup;
};

ProcessControlEvents &processControlEvents() {
  // Control callbacks can outlive handler unregistration. The kernel reclaims
  // these process-lifetime handles only after no callback can still use them.
  static ProcessControlEvents events;
  return events;
}

std::optional<TerminationRequest> terminationRequest(DWORD control) {
  switch (control) {
  case CTRL_C_EVENT:
    return TerminationRequest{TerminationKind::Interrupt};
  case CTRL_BREAK_EVENT:
    return TerminationRequest{TerminationKind::Break};
  case CTRL_CLOSE_EVENT:
    return TerminationRequest{TerminationKind::Close};
  case CTRL_LOGOFF_EVENT:
    return TerminationRequest{TerminationKind::Logoff};
  case CTRL_SHUTDOWN_EVENT:
    return TerminationRequest{TerminationKind::Shutdown};
  default:
    return std::nullopt;
  }
}

bool waitsForCleanup(DWORD control) {
  return control == CTRL_CLOSE_EVENT || control == CTRL_LOGOFF_EVENT ||
         control == CTRL_SHUTDOWN_EVENT;
}

BOOL WINAPI handleConsoleControl(DWORD control) {
  if (!terminationRequest(control)) {
    return FALSE;
  }
  const HANDLE controlEvent = activeControlEvent.load();
  if (controlEvent == nullptr) {
    return FALSE;
  }

  DWORD expected = noControlRequest;
  (void)activeControlRequest.compare_exchange_strong(expected, control);
  if (::SetEvent(controlEvent) == 0) {
    return FALSE;
  }
  if (waitsForCleanup(control)) {
    const HANDLE cleanupEvent = activeCleanupEvent.load();
    if (cleanupEvent != nullptr) {
      (void)::WaitForSingleObject(cleanupEvent, cleanupWaitMilliseconds);
    }
  }
  return TRUE;
}

class WaitDeadline {
public:
  explicit WaitDeadline(std::optional<std::chrono::milliseconds> timeout) {
    if (timeout) {
      deadline_ = std::chrono::steady_clock::now() +
                  std::max(*timeout, std::chrono::milliseconds::zero());
    }
  }

  [[nodiscard]] DWORD remaining() const {
    if (!deadline_) {
      return INFINITE;
    }
    const auto duration = *deadline_ - std::chrono::steady_clock::now();
    if (duration <= std::chrono::steady_clock::duration::zero()) {
      return 0;
    }
    const auto milliseconds =
        std::chrono::ceil<std::chrono::milliseconds>(duration).count();
    return static_cast<DWORD>(std::min<std::int64_t>(
        milliseconds, static_cast<std::int64_t>(INFINITE - 1)));
  }

private:
  std::optional<std::chrono::steady_clock::time_point> deadline_;
};

bool ready(const PlatformReadiness &readiness) {
  return readiness.input || readiness.resize || readiness.termination ||
         !readiness.wakes.empty();
}

bool signaled(HANDLE handle, const char *message) {
  const DWORD result = ::WaitForSingleObject(handle, 0);
  if (result == WAIT_OBJECT_0)
    return true;
  if (result == WAIT_TIMEOUT)
    return false;
  if (result == WAIT_FAILED)
    throwLastError(message);
  throw std::runtime_error{message};
}

} // namespace

struct PlatformWake::Impl {
  Impl() : event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)} {
    if (event == nullptr)
      throwLastError("failed to create platform wake");
  }
  ~Impl() { (void)::CloseHandle(event); }

  HANDLE event;
};

PlatformWake::PlatformWake() : impl_{std::make_unique<Impl>()} {}
PlatformWake::~PlatformWake() = default;
PlatformWake::PlatformWake(PlatformWake &&) noexcept = default;
PlatformWake &PlatformWake::operator=(PlatformWake &&) noexcept = default;

void PlatformWake::notify() noexcept { (void)::SetEvent(impl_->event); }
void PlatformWake::consume() noexcept { (void)::ResetEvent(impl_->event); }

struct PlatformEventLoop::Impl {
  explicit Impl(PlatformEventLoopOptions configuredOptions)
      : options{configuredOptions} {
    if (options.monitorInput) {
      input = ::GetStdHandle(STD_INPUT_HANDLE);
      output = ::GetStdHandle(STD_OUTPUT_HANDLE);
      DWORD mode = 0;
      CONSOLE_SCREEN_BUFFER_INFO information{};
      if (input == nullptr || input == INVALID_HANDLE_VALUE ||
          ::GetConsoleMode(input, &mode) == 0) {
        throwLastError("failed to acquire console input");
      }
      if (output == nullptr || output == INVALID_HANDLE_VALUE ||
          ::GetConsoleScreenBufferInfo(output, &information) == 0) {
        throwLastError("failed to acquire console output");
      }
    }
    if (options.monitorProcessControl) {
      auto &events = processControlEvents();
      controlEvent = events.control;
      cleanupEvent = events.cleanup;
      if (::ResetEvent(controlEvent) == 0 || ::ResetEvent(cleanupEvent) == 0) {
        throwLastError("failed to reset console control events");
      }

      HANDLE expected = nullptr;
      if (!activeControlEvent.compare_exchange_strong(expected, controlEvent)) {
        throw std::logic_error{
            "only one process-control event loop may be active"};
      }
      activeCleanupEvent.store(cleanupEvent);
      activeControlRequest.store(noControlRequest);
      if (::SetConsoleCtrlHandler(handleConsoleControl, TRUE) == 0) {
        const int error = static_cast<int>(::GetLastError());
        clearActiveControl();
        throw std::system_error{error, std::system_category(),
                                "failed to install console control handler"};
      }
      controlHandlerInstalled = true;
    }
  }

  ~Impl() {
    if (controlHandlerInstalled) {
      (void)::SetEvent(cleanupEvent);
      (void)::SetConsoleCtrlHandler(handleConsoleControl, FALSE);
      clearActiveControl();
    }
  }

  void clearActiveControl() noexcept {
    activeControlRequest.store(noControlRequest);
    activeCleanupEvent.store(nullptr);
    HANDLE expected = controlEvent;
    (void)activeControlEvent.compare_exchange_strong(expected, nullptr);
  }

  [[nodiscard]] COORD visibleWindowOrigin() const {
    CONSOLE_SCREEN_BUFFER_INFO information{};
    if (::GetConsoleScreenBufferInfo(output, &information) == 0) {
      throwLastError("failed to query the visible console window");
    }
    return {information.srWindow.Left, information.srWindow.Top};
  }

  void drainConsole(PlatformReadiness &readiness) {
    std::array<INPUT_RECORD, 128> records{};
    for (;;) {
      DWORD available = 0;
      if (::GetNumberOfConsoleInputEvents(input, &available) == 0) {
        throwLastError("failed to inspect console input");
      }
      if (available == 0) {
        return;
      }

      DWORD count = 0;
      const DWORD requested =
          std::min<DWORD>(available, static_cast<DWORD>(records.size()));
      // ReadConsoleInputW is the only consumer of the Windows console queue.
      // Translated bytes remain owned here until readInput consumes them.
      if (::ReadConsoleInputW(input, records.data(), requested, &count) == 0) {
        throwLastError("failed to read console input");
      }
      if (count == 0) {
        throw std::runtime_error{"console input read returned no records"};
      }
      const auto translated = translator.translate(
          std::span<const INPUT_RECORD>{records.data(), count},
          visibleWindowOrigin());
      inputBytes += translated.bytes;
      readiness.resize = readiness.resize || translated.resize;
    }
  }

  PlatformEventLoopOptions options;
  HANDLE input = INVALID_HANDLE_VALUE;
  HANDLE output = INVALID_HANDLE_VALUE;
  HANDLE controlEvent = nullptr;
  HANDLE cleanupEvent = nullptr;
  bool controlHandlerInstalled = false;
  WindowsConsoleInputTranslator translator;
  std::string inputBytes;
};

PlatformEventLoop::PlatformEventLoop(PlatformEventLoopOptions options)
    : impl_{std::make_unique<Impl>(options)} {}
PlatformEventLoop::~PlatformEventLoop() = default;

PlatformReadiness
PlatformEventLoop::wait(std::optional<std::chrono::milliseconds> timeout,
                        std::span<const PlatformWake *const> wakes) {
  WaitDeadline deadline{timeout};
  for (;;) {
    std::vector<HANDLE> handles;
    std::optional<std::size_t> inputIndex;
    std::optional<std::size_t> controlIndex;
    std::vector<std::optional<std::size_t>> wakeIndices(wakes.size());
    const auto add = [&](HANDLE handle) {
      handles.push_back(handle);
      return handles.size() - 1;
    };
    if (impl_->options.monitorInput) {
      inputIndex = add(impl_->input);
    }
    if (impl_->controlEvent != nullptr) {
      controlIndex = add(impl_->controlEvent);
    }
    for (std::size_t index = 0; index < wakes.size(); ++index) {
      if (wakes[index] != nullptr) {
        wakeIndices[index] = add(wakes[index]->impl_->event);
      }
    }

    PlatformReadiness readiness;
    readiness.input = !impl_->inputBytes.empty();
    if (handles.empty()) {
      if (readiness.input) {
        return readiness;
      }
      const DWORD remaining = deadline.remaining();
      if (remaining == INFINITE) {
        throw std::logic_error{
            "an indefinite platform wait requires an input or wake handle"};
      }
      ::Sleep(remaining);
      return {};
    }
    if (handles.size() > MAXIMUM_WAIT_OBJECTS) {
      throw std::length_error{"too many handles in platform wait"};
    }

    const DWORD result = ::WaitForMultipleObjects(
        static_cast<DWORD>(handles.size()), handles.data(), FALSE,
        readiness.input ? 0 : deadline.remaining());
    if (result == WAIT_FAILED) {
      throwLastError("platform wait failed");
    }
    if (result != WAIT_TIMEOUT &&
        (result < WAIT_OBJECT_0 ||
         result >= WAIT_OBJECT_0 + static_cast<DWORD>(handles.size()))) {
      throw std::runtime_error{"platform wait returned an invalid result"};
    }

    if (result != WAIT_TIMEOUT) {
      if (inputIndex &&
          signaled(handles[*inputIndex], "failed to inspect console input")) {
        impl_->drainConsole(readiness);
      }
      if (controlIndex &&
          signaled(handles[*controlIndex],
                   "failed to inspect console control readiness")) {
        readiness.termination = terminationRequest(activeControlRequest.load());
      }
      for (std::size_t index = 0; index < wakes.size(); ++index) {
        if (wakeIndices[index] && signaled(handles[*wakeIndices[index]],
                                           "failed to inspect platform wake")) {
          readiness.wakes.push_back(index);
        }
      }
    }
    readiness.input = !impl_->inputBytes.empty();
    if (ready(readiness) || result == WAIT_TIMEOUT) {
      return readiness;
    }
  }
}

std::size_t PlatformEventLoop::readInput(std::span<char> destination) {
  if (!impl_->options.monitorInput) {
    throw std::logic_error{"standard input monitoring is disabled"};
  }
  if (destination.empty()) {
    throw std::invalid_argument{"standard input destination is empty"};
  }
  if (impl_->inputBytes.empty()) {
    throw std::logic_error{"no translated console input is ready"};
  }
  const std::size_t count =
      std::min(destination.size(), impl_->inputBytes.size());
  std::copy_n(impl_->inputBytes.data(), count, destination.data());
  impl_->inputBytes.erase(0, count);
  return count;
}

std::uint64_t processId() noexcept {
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
}

std::chrono::nanoseconds monotonicTime() {
  LARGE_INTEGER frequency{};
  LARGE_INTEGER counter{};
  if (::QueryPerformanceFrequency(&frequency) == 0 ||
      ::QueryPerformanceCounter(&counter) == 0) {
    throwLastError("failed to read monotonic clock");
  }
  const auto seconds = counter.QuadPart / frequency.QuadPart;
  const auto remainder = counter.QuadPart % frequency.QuadPart;
  return std::chrono::seconds{seconds} +
         std::chrono::nanoseconds{remainder * 1'000'000'000LL /
                                  frequency.QuadPart};
}

[[noreturn]] void terminateProcess(TerminationRequest request) {
  if (const HANDLE cleanupEvent = activeCleanupEvent.load();
      cleanupEvent != nullptr) {
    (void)::SetEvent(cleanupEvent);
  }
  const UINT status = request.kind == TerminationKind::Interrupt ||
                              request.kind == TerminationKind::Break
                          ? controlExitStatus
                          : ERROR_PROCESS_ABORTED;
  ::ExitProcess(status);
}

} // namespace ssg
