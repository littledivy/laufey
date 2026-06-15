// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Windows-VK → W3C key/code mapping. Used by CEF on every platform
// (CEF normalizes keyboard events to Windows VK codes) and by the
// webview Windows backend.

#include "laufey_backend_common.h"

#include <cctype>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace laufey_common {

// Windows VK constants — defined inline so this file compiles on
// non-Windows platforms (CEF Mac / Linux use the same mapping because
// CEF normalizes to Windows VK).
namespace {
constexpr int K_BACK = 0x08;
constexpr int K_TAB = 0x09;
constexpr int K_RETURN = 0x0D;
constexpr int K_SHIFT = 0x10;
constexpr int K_CONTROL = 0x11;
constexpr int K_MENU = 0x12;  // Alt
constexpr int K_PAUSE = 0x13;
constexpr int K_CAPITAL = 0x14;
constexpr int K_ESCAPE = 0x1B;
constexpr int K_SPACE = 0x20;
constexpr int K_PRIOR = 0x21;  // Page Up
constexpr int K_NEXT = 0x22;   // Page Down
constexpr int K_END = 0x23;
constexpr int K_HOME = 0x24;
constexpr int K_LEFT = 0x25;
constexpr int K_UP = 0x26;
constexpr int K_RIGHT = 0x27;
constexpr int K_DOWN = 0x28;
constexpr int K_INSERT = 0x2D;
constexpr int K_DELETE = 0x2E;
constexpr int K_LWIN = 0x5B;
constexpr int K_RWIN = 0x5C;
constexpr int K_APPS = 0x5D;  // CEF legacy maps this to Meta too
constexpr int K_F1 = 0x70;
constexpr int K_F12 = 0x7B;
constexpr int K_NUMLOCK = 0x90;
constexpr int K_SCROLL = 0x91;
constexpr int K_OEM_1 = 0xBA;       // ;
constexpr int K_OEM_PLUS = 0xBB;    // =
constexpr int K_OEM_COMMA = 0xBC;   // ,
constexpr int K_OEM_MINUS = 0xBD;   // -
constexpr int K_OEM_PERIOD = 0xBE;  // .
constexpr int K_OEM_2 = 0xBF;       // /
constexpr int K_OEM_3 = 0xC0;       // `
constexpr int K_OEM_4 = 0xDB;       // [
constexpr int K_OEM_5 = 0xDC;       /* \ */
constexpr int K_OEM_6 = 0xDD;       // ]
constexpr int K_OEM_7 = 0xDE;       // '

#ifdef _WIN32
constexpr int K_RSHIFT = 0xA1;
#endif

}  // namespace

std::string VkToKey(int vk, uint32_t character, bool shift_held,
                    bool caps_on) {
  // If a printable ASCII char came in, use it directly. Matches CEF's
  // OnKeyEvent path, which forwards `event.character` here.
  if (character > 0 && character < 0x7F &&
      isprint(static_cast<int>(character))) {
    return std::string(1, static_cast<char>(character));
  }
  switch (vk) {
    case K_BACK: return "Backspace";
    case K_TAB: return "Tab";
    case K_RETURN: return "Enter";
    case K_SHIFT: return "Shift";
    case K_CONTROL: return "Control";
    case K_MENU: return "Alt";
    case K_PAUSE: return "Pause";
    case K_CAPITAL: return "CapsLock";
    case K_ESCAPE: return "Escape";
    case K_SPACE: return " ";
    case K_PRIOR: return "PageUp";
    case K_NEXT: return "PageDown";
    case K_END: return "End";
    case K_HOME: return "Home";
    case K_LEFT: return "ArrowLeft";
    case K_UP: return "ArrowUp";
    case K_RIGHT: return "ArrowRight";
    case K_DOWN: return "ArrowDown";
    case K_INSERT: return "Insert";
    case K_DELETE: return "Delete";
    case K_LWIN:
    case K_RWIN:
    case K_APPS:  // CEF legacy
      return "Meta";
    case K_NUMLOCK: return "NumLock";
    case K_SCROLL: return "ScrollLock";
  }
  if (vk >= K_F1 && vk <= K_F12) {
    return "F" + std::to_string(vk - K_F1 + 1);
  }
  if (vk >= 'A' && vk <= 'Z') {
    char c = static_cast<char>(vk);
    if (!(shift_held ^ caps_on))
      c = static_cast<char>(c + 32);  // lowercase
    return std::string(1, c);
  }
  if (vk >= '0' && vk <= '9') {
    return std::string(1, static_cast<char>(vk));
  }
  return "Unidentified";
}

std::string VkToCode(int vk, bool is_extended, uint32_t scancode) {
  switch (vk) {
    case K_BACK: return "Backspace";
    case K_TAB: return "Tab";
    case K_RETURN: return is_extended ? "NumpadEnter" : "Enter";
    case K_SHIFT: {
#ifdef _WIN32
      if (scancode != 0) {
        if (MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX) == K_RSHIFT)
          return "ShiftRight";
      }
#else
      (void)scancode;
#endif
      return "ShiftLeft";
    }
    case K_CONTROL: return is_extended ? "ControlRight" : "ControlLeft";
    case K_MENU: return is_extended ? "AltRight" : "AltLeft";
    case K_PAUSE: return "Pause";
    case K_CAPITAL: return "CapsLock";
    case K_ESCAPE: return "Escape";
    case K_SPACE: return "Space";
    case K_PRIOR: return "PageUp";
    case K_NEXT: return "PageDown";
    case K_END: return "End";
    case K_HOME: return "Home";
    case K_LEFT: return "ArrowLeft";
    case K_UP: return "ArrowUp";
    case K_RIGHT: return "ArrowRight";
    case K_DOWN: return "ArrowDown";
    case K_INSERT: return "Insert";
    case K_DELETE: return "Delete";
    case K_LWIN: return "MetaLeft";
    case K_RWIN: return "MetaRight";
    case K_APPS: return "MetaRight";  // CEF legacy
    case K_NUMLOCK: return "NumLock";
    case K_SCROLL: return "ScrollLock";
    case K_OEM_1: return "Semicolon";
    case K_OEM_PLUS: return "Equal";
    case K_OEM_COMMA: return "Comma";
    case K_OEM_MINUS: return "Minus";
    case K_OEM_PERIOD: return "Period";
    case K_OEM_2: return "Slash";
    case K_OEM_3: return "Backquote";
    case K_OEM_4: return "BracketLeft";
    case K_OEM_5: return "Backslash";
    case K_OEM_6: return "BracketRight";
    case K_OEM_7: return "Quote";
  }
  if (vk >= K_F1 && vk <= K_F12) {
    return "F" + std::to_string(vk - K_F1 + 1);
  }
  if (vk >= 'A' && vk <= 'Z') {
    return "Key" + std::string(1, static_cast<char>(vk));
  }
  if (vk >= '0' && vk <= '9') {
    return "Digit" + std::string(1, static_cast<char>(vk));
  }
  return "Unidentified";
}

}  // namespace laufey_common
