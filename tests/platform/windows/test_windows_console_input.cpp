#include "fixtures/platform_input_cases.h"
#include "test_helpers.h"

#include "WindowsConsoleInput.h"

#include <array>
#include <span>
#include <vector>

namespace {

INPUT_RECORD key(WORD virtualKey, wchar_t text = L'\0', DWORD modifiers = 0,
                 WORD repeat = 1, bool down = true) {
  INPUT_RECORD record{};
  record.EventType = KEY_EVENT;
  record.Event.KeyEvent.bKeyDown = down;
  record.Event.KeyEvent.wRepeatCount = repeat;
  record.Event.KeyEvent.wVirtualKeyCode = virtualKey;
  record.Event.KeyEvent.uChar.UnicodeChar = text;
  record.Event.KeyEvent.dwControlKeyState = modifiers;
  return record;
}

INPUT_RECORD mouse(SHORT column, SHORT row, DWORD buttons, DWORD flags = 0,
                   DWORD modifiers = 0) {
  INPUT_RECORD record{};
  record.EventType = MOUSE_EVENT;
  record.Event.MouseEvent.dwMousePosition = {column, row};
  record.Event.MouseEvent.dwButtonState = buttons;
  record.Event.MouseEvent.dwControlKeyState = modifiers;
  record.Event.MouseEvent.dwEventFlags = flags;
  return record;
}

std::vector<INPUT_RECORD> records(ssg::test::PlatformInputCaseId id) {
  using Id = ssg::test::PlatformInputCaseId;
  switch (id) {
  case Id::LowerA:
    return {key('A', L'a')};
  case Id::UpperA:
    return {key('A', L'A', SHIFT_PRESSED)};
  case Id::Emoji:
    return {key(0, static_cast<wchar_t>(0xd83d)),
            key(0, static_cast<wchar_t>(0xde00))};
  case Id::Enter:
    return {key(VK_RETURN, L'\r')};
  case Id::ControlC:
    return {key('C', static_cast<wchar_t>(3), LEFT_CTRL_PRESSED)};
  case Id::AltShiftP:
    return {key('P', L'P', LEFT_ALT_PRESSED | SHIFT_PRESSED)};
  case Id::ArrowLeft:
    return {key(VK_LEFT)};
  case Id::ControlArrowRight:
    return {key(VK_RIGHT, L'\0', LEFT_CTRL_PRESSED)};
  case Id::ControlAltArrowRight:
    return {key(VK_RIGHT, L'\0', LEFT_CTRL_PRESSED | LEFT_ALT_PRESSED)};
  case Id::Delete:
    return {key(VK_DELETE)};
  case Id::PointerPress:
    return {mouse(12, 23, FROM_LEFT_1ST_BUTTON_PRESSED)};
  case Id::PointerRelease:
    return {mouse(12, 23, 0)};
  case Id::PointerDrag:
    return {mouse(14, 25, FROM_LEFT_1ST_BUTTON_PRESSED, MOUSE_MOVED,
                  LEFT_ALT_PRESSED)};
  case Id::WheelUp:
    return {mouse(16, 27,
                  static_cast<DWORD>(static_cast<WORD>(WHEEL_DELTA)) << 16,
                  MOUSE_WHEELED)};
  case Id::AltWheelUp:
    return {mouse(16, 27,
                  static_cast<DWORD>(static_cast<WORD>(WHEEL_DELTA)) << 16,
                  MOUSE_WHEELED, LEFT_ALT_PRESSED)};
  case Id::MetaP:
    return {};
  }
  return {};
}

TEST(windowsRecordsProduceCanonicalSharedInput) {
  constexpr COORD origin{10, 20};
  for (const auto &input : ssg::test::platformInputCases()) {
    if (!input.windowsRecord)
      continue;
    ssg::WindowsConsoleInputTranslator translator;
    if (input.id == ssg::test::PlatformInputCaseId::PointerRelease ||
        input.id == ssg::test::PlatformInputCaseId::PointerDrag) {
      const auto pressed = mouse(12, 23, FROM_LEFT_1ST_BUTTON_PRESSED);
      const auto priming = translator.translate({&pressed, 1}, origin);
      ASSERT_EQ(priming.bytes, std::string{"\x1b[<0;3;4M"});
    }
    const auto translated = translator.translate(records(input.id), origin);
    ASSERT_EQ(translated.bytes, std::string{input.bytes});
    ASSERT_FALSE(translated.resize);

    std::size_t consumed = 0;
    const auto decoded = ssg::decodeInput(translated.bytes, true, consumed);
    ASSERT_EQ(consumed, translated.bytes.size());
    ASSERT_TRUE(ssg::test::sameDecoded(decoded, input.expected));
  }
}

TEST(textRepeatsAndSplitSurrogatePairsStayWhole) {
  ssg::WindowsConsoleInputTranslator translator;
  const auto repeatedKey = key('X', L'x', 0, 3);
  const auto repeated = translator.translate({&repeatedKey, 1}, {});
  ASSERT_EQ(repeated.bytes, std::string{"xxx"});

  const auto high = key(0, static_cast<wchar_t>(0xd83d));
  const auto low = key(0, static_cast<wchar_t>(0xde00));
  ASSERT_TRUE(translator.translate({&high, 1}, {}).bytes.empty());
  ASSERT_EQ(translator.translate({&low, 1}, {}).bytes,
            std::string{"\xf0\x9f\x98\x80"});

  const auto repeatedHigh = key(0, static_cast<wchar_t>(0xd83d), 0, 2);
  const auto repeatedLow = key(0, static_cast<wchar_t>(0xde00), 0, 2);
  ASSERT_TRUE(translator.translate({&repeatedHigh, 1}, {}).bytes.empty());
  ASSERT_EQ(translator.translate({&repeatedLow, 1}, {}).bytes,
            std::string{"\xf0\x9f\x98\x80\xf0\x9f\x98\x80"});

  const auto mismatchedHigh =
      key(0, static_cast<wchar_t>(0xd83d), LEFT_ALT_PRESSED, 0);
  const auto mismatchedLow = key(0, static_cast<wchar_t>(0xde00), 0, 3);
  ASSERT_TRUE(translator.translate({&mismatchedHigh, 1}, {}).bytes.empty());
  ASSERT_EQ(translator.translate({&mismatchedLow, 1}, {}).bytes,
            std::string{"\xf0\x9f\x98\x80"});
}

TEST(altGrCommitsTextWithoutCreatingAChord) {
  ssg::WindowsConsoleInputTranslator translator;
  const auto record = key('Q', L'@', RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED);
  const auto translated = translator.translate({&record, 1}, {});
  ASSERT_EQ(translated.bytes, std::string{"@"});

  std::size_t consumed = 0;
  const auto decoded = ssg::decodeInput(translated.bytes, true, consumed);
  ASSERT_EQ(decoded.text, std::string{"@"});
  ASSERT_FALSE(decoded.stroke.mod);
}

TEST(malformedAndIgnorableRecordsProduceNoInput) {
  ssg::WindowsConsoleInputTranslator translator;
  const auto low = key(0, static_cast<wchar_t>(0xde00));
  ASSERT_TRUE(translator.translate({&low, 1}, {}).bytes.empty());

  const auto high = key(0, static_cast<wchar_t>(0xd83d));
  const auto letter = key('A', L'a');
  std::array malformed{high, letter};
  ASSERT_EQ(translator.translate(malformed, {}).bytes, std::string{"a"});

  std::array<INPUT_RECORD, 3> ignored{};
  ignored[0] = key('A', L'a', 0, 1, false);
  ignored[1].EventType = FOCUS_EVENT;
  ignored[2].EventType = MENU_EVENT;
  ASSERT_TRUE(translator.translate(ignored, {}).bytes.empty());
}

TEST(bufferSizeRecordsReportResizeWithoutInput) {
  ssg::WindowsConsoleInputTranslator translator;
  INPUT_RECORD resize{};
  resize.EventType = WINDOW_BUFFER_SIZE_EVENT;
  resize.Event.WindowBufferSizeEvent.dwSize = {120, 40};
  const auto translated = translator.translate({&resize, 1}, {});
  ASSERT_TRUE(translated.resize);
  ASSERT_TRUE(translated.bytes.empty());
}

TEST(mouseTransitionsPreserveReleasedButtonAndAltWheel) {
  ssg::WindowsConsoleInputTranslator translator;
  const auto left = mouse(2, 3, FROM_LEFT_1ST_BUTTON_PRESSED);
  ASSERT_EQ(translator.translate({&left, 1}, {}).bytes,
            std::string{"\x1b[<0;3;4M"});

  const auto both =
      mouse(2, 3, FROM_LEFT_1ST_BUTTON_PRESSED | RIGHTMOST_BUTTON_PRESSED);
  ASSERT_EQ(translator.translate({&both, 1}, {}).bytes,
            std::string{"\x1b[<2;3;4M"});

  ASSERT_EQ(translator.translate({&left, 1}, {}).bytes,
            std::string{"\x1b[<2;3;4m"});

  const auto movedRelease = mouse(4, 5, 0, MOUSE_MOVED);
  ASSERT_EQ(translator.translate({&movedRelease, 1}, {}).bytes,
            std::string{"\x1b[<0;5;6m"});
}

} // namespace

SSG_TEST_SUITE(test_windows_console_input) {
  RUN(windowsRecordsProduceCanonicalSharedInput);
  RUN(textRepeatsAndSplitSurrogatePairsStayWhole);
  RUN(altGrCommitsTextWithoutCreatingAChord);
  RUN(malformedAndIgnorableRecordsProduceNoInput);
  RUN(bufferSizeRecordsReportResizeWithoutInput);
  RUN(mouseTransitionsPreserveReleasedButtonAndAltWheel);
  return failed == 0 ? 0 : 1;
}
