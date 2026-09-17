#pragma once

#include <ssg/TerminalInput.h>

#include <string_view>
#include <utility>
#include <vector>

namespace ssg::test {

enum class PlatformInputCaseId {
  LowerA,
  UpperA,
  Emoji,
  Enter,
  ControlC,
  AltShiftP,
  MetaP,
  ArrowLeft,
  ControlArrowRight,
  ControlAltArrowRight,
  Delete,
  PointerPress,
  PointerRelease,
  PointerDrag,
  WheelUp,
  AltWheelUp,
};

struct PlatformInputCase {
  PlatformInputCaseId id;
  std::string_view bytes;
  Decoded expected;
  bool windowsRecord = true;
};

inline Decoded key(KeyCode code, bool mod = false, bool meta = false,
                   bool shift = false, std::string text = {}) {
  Decoded value;
  value.status = DecodeStatus::key;
  value.stroke = {code, mod, meta, shift};
  value.text = std::move(text);
  return value;
}

inline std::vector<PlatformInputCase> platformInputCases() {
  Decoded emoji = key(KeyCode::None, false, false, false, "\xf0\x9f\x98\x80");

  Decoded press;
  press.status = DecodeStatus::pointer;
  press.pointer = {2, 3, PointerButton::left, PointerKind::press, false};
  Decoded release = press;
  release.pointer.kind = PointerKind::release;
  Decoded drag = press;
  drag.pointer = {4, 5, PointerButton::left, PointerKind::drag, true};
  Decoded wheel;
  wheel.status = DecodeStatus::scroll;
  wheel.scroll = -3;
  wheel.pointer.column = 6;
  wheel.pointer.row = 7;
  Decoded altWheel = wheel;
  altWheel.pointer.alt = true;

  return {
      {PlatformInputCaseId::LowerA, "a",
       key(KeyCode::KeyA, false, false, false, "a")},
      {PlatformInputCaseId::UpperA, "A",
       key(KeyCode::KeyA, false, false, true, "A")},
      {PlatformInputCaseId::Emoji, "\xf0\x9f\x98\x80", std::move(emoji)},
      {PlatformInputCaseId::Enter, "\r", key(KeyCode::Enter)},
      {PlatformInputCaseId::ControlC, "\x1b[99;5u", key(KeyCode::KeyC, true)},
      {PlatformInputCaseId::AltShiftP, "\x1b[112;4u",
       key(KeyCode::KeyP, true, false, true)},
      {PlatformInputCaseId::MetaP, "\x1b[112;33u",
       key(KeyCode::KeyP, false, true), false},
      {PlatformInputCaseId::ArrowLeft, "\x1b[D", key(KeyCode::ArrowLeft)},
      {PlatformInputCaseId::ControlArrowRight, "\x1b[1;5C",
       key(KeyCode::ArrowRight, true)},
      {PlatformInputCaseId::ControlAltArrowRight, "\x1b[1;7C",
       key(KeyCode::ArrowRight)},
      {PlatformInputCaseId::Delete, "\x1b[3~", key(KeyCode::Delete)},
      {PlatformInputCaseId::PointerPress, "\x1b[<0;3;4M", std::move(press)},
      {PlatformInputCaseId::PointerRelease, "\x1b[<0;3;4m", std::move(release)},
      {PlatformInputCaseId::PointerDrag, "\x1b[<40;5;6M", std::move(drag)},
      {PlatformInputCaseId::WheelUp, "\x1b[<64;7;8M", std::move(wheel)},
      {PlatformInputCaseId::AltWheelUp, "\x1b[<72;7;8M", std::move(altWheel)},
  };
}

inline bool sameDecoded(const Decoded &left, const Decoded &right) {
  return left.status == right.status && left.stroke == right.stroke &&
         left.text == right.text && left.scroll == right.scroll &&
         left.pointer == right.pointer && left.reply == right.reply;
}

} // namespace ssg::test
