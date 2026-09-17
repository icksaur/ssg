#include "test_helpers.h"

#include "WindowsTerminal.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class Operation { InputMode, OutputMode, CodePage };

struct ConsoleState {
  DWORD inputMode = ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT |
                    ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE |
                    ENABLE_VIRTUAL_TERMINAL_INPUT;
  DWORD outputMode = ENABLE_WRAP_AT_EOL_OUTPUT;
  UINT codePage = 437;
  std::vector<std::pair<Operation, DWORD>> changes;
  std::string written;
  std::optional<Operation> fail;
};

class FakeConsole final : public ssg::WindowsConsoleApi {
public:
  explicit FakeConsole(ConsoleState &state) : state_{state} {}

  bool inputMode(DWORD &mode) override {
    mode = state_.inputMode;
    return true;
  }
  bool outputMode(DWORD &mode) override {
    mode = state_.outputMode;
    return true;
  }
  bool setInputMode(DWORD mode) override {
    state_.changes.emplace_back(Operation::InputMode, mode);
    if (state_.fail == Operation::InputMode)
      return false;
    state_.inputMode = mode;
    return true;
  }
  bool setOutputMode(DWORD mode) override {
    state_.changes.emplace_back(Operation::OutputMode, mode);
    if (state_.fail == Operation::OutputMode)
      return false;
    state_.outputMode = mode;
    return true;
  }
  UINT outputCodePage() override { return state_.codePage; }
  bool setOutputCodePage(UINT codePage) override {
    state_.changes.emplace_back(Operation::CodePage, codePage);
    if (state_.fail == Operation::CodePage)
      return false;
    state_.codePage = codePage;
    return true;
  }
  bool write(std::span<const char> bytes) override {
    state_.written.append(bytes.data(), bytes.size());
    return true;
  }
  bool screenSize(CONSOLE_SCREEN_BUFFER_INFO &) override { return true; }

private:
  ConsoleState &state_;
};

TEST(windowsTerminalActivatesAndRestoresExactState) {
  ConsoleState state;
  const DWORD originalInput = state.inputMode;
  const DWORD originalOutput = state.outputMode;
  const UINT originalCodePage = state.codePage;
  auto terminal =
      ssg::makeWindowsTerminal(std::make_unique<FakeConsole>(state));

  ASSERT_TRUE(terminal->activate());
  ASSERT_TRUE((state.inputMode & ENABLE_WINDOW_INPUT) != 0);
  ASSERT_TRUE((state.inputMode & ENABLE_MOUSE_INPUT) != 0);
  ASSERT_TRUE((state.inputMode & ENABLE_EXTENDED_FLAGS) != 0);
  ASSERT_EQ(state.inputMode & (ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT |
                               ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                               ENABLE_VIRTUAL_TERMINAL_INPUT),
            DWORD{0});
  ASSERT_TRUE((state.outputMode & ENABLE_PROCESSED_OUTPUT) != 0);
  ASSERT_TRUE((state.outputMode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0);
  ASSERT_EQ(state.codePage, UINT{CP_UTF8});

  terminal->write("utf8");
  ASSERT_EQ(state.written, std::string{"utf8"});
  ASSERT_FALSE(terminal->supportsKeyboardProtocol());
  terminal->restore();
  ASSERT_EQ(state.inputMode, originalInput);
  ASSERT_EQ(state.outputMode, originalOutput);
  ASSERT_EQ(state.codePage, originalCodePage);
  ASSERT_EQ(state.changes.size(), std::size_t{6});
  ASSERT_EQ(
      state.changes[3],
      (std::pair{Operation::CodePage, static_cast<DWORD>(originalCodePage)}));
  ASSERT_EQ(state.changes[4],
            (std::pair{Operation::OutputMode, originalOutput}));
  ASSERT_EQ(state.changes[5], (std::pair{Operation::InputMode, originalInput}));
  const auto restoredChanges = state.changes.size();
  terminal->restore();
  ASSERT_EQ(state.changes.size(), restoredChanges);
}

TEST(partialActivationRollsBackEverySuccessfulMutation) {
  for (const auto failure :
       {Operation::InputMode, Operation::OutputMode, Operation::CodePage}) {
    ConsoleState state;
    const DWORD originalInput = state.inputMode;
    const DWORD originalOutput = state.outputMode;
    const UINT originalCodePage = state.codePage;
    state.fail = failure;
    auto terminal =
        ssg::makeWindowsTerminal(std::make_unique<FakeConsole>(state));

    ASSERT_FALSE(terminal->activate());
    ASSERT_EQ(state.inputMode, originalInput);
    ASSERT_EQ(state.outputMode, originalOutput);
    ASSERT_EQ(state.codePage, originalCodePage);
    const auto expectedChanges =
        failure == Operation::InputMode    ? std::size_t{1}
        : failure == Operation::OutputMode ? std::size_t{3}
                                           : std::size_t{5};
    ASSERT_EQ(state.changes.size(), expectedChanges);
    state.fail.reset();
    const auto restoredChanges = state.changes.size();
    terminal->restore();
    ASSERT_EQ(state.inputMode, originalInput);
    ASSERT_EQ(state.outputMode, originalOutput);
    ASSERT_EQ(state.codePage, originalCodePage);
    ASSERT_EQ(state.changes.size(), restoredChanges);
  }
}

TEST(windowsSessionDoesNotRequestKittyKeyboardMode) {
  ConsoleState state;
  ssg::TerminalSession session{
      ssg::makeWindowsTerminal(std::make_unique<FakeConsole>(state))};
  ASSERT_TRUE(session.active());
  state.written.clear();
  session.enableKeyboardProtocol();
  ASSERT_TRUE(state.written.empty());
}

} // namespace

SSG_TEST_SUITE(test_windows_terminal) {
  RUN(windowsTerminalActivatesAndRestoresExactState);
  RUN(partialActivationRollsBackEverySuccessfulMutation);
  RUN(windowsSessionDoesNotRequestKittyKeyboardMode);
  return failed == 0 ? 0 : 1;
}
