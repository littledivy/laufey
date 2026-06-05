// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// NSEvent → W3C key/code mapping. Used only by the webview macOS
// backend (CEF normalizes keyboard events to Windows VK codes and uses
// keymap_vk.cc on every platform).

#include "laufey_backend_common.h"

#import <AppKit/AppKit.h>

#include <string>

namespace laufey_common {

std::string NSEventKeyToKey(void* event_nsevent) {
  NSEvent* event = (__bridge NSEvent*)event_nsevent;
  NSString* chars = [event characters];
  if (!chars || [chars length] == 0) return "Unidentified";
  unichar c = [chars characterAtIndex:0];
  switch (c) {
    case NSUpArrowFunctionKey: return "ArrowUp";
    case NSDownArrowFunctionKey: return "ArrowDown";
    case NSLeftArrowFunctionKey: return "ArrowLeft";
    case NSRightArrowFunctionKey: return "ArrowRight";
    case NSHomeFunctionKey: return "Home";
    case NSEndFunctionKey: return "End";
    case NSPageUpFunctionKey: return "PageUp";
    case NSPageDownFunctionKey: return "PageDown";
    case NSDeleteFunctionKey: return "Delete";
    case NSInsertFunctionKey: return "Insert";
    case NSF1FunctionKey: return "F1";
    case NSF2FunctionKey: return "F2";
    case NSF3FunctionKey: return "F3";
    case NSF4FunctionKey: return "F4";
    case NSF5FunctionKey: return "F5";
    case NSF6FunctionKey: return "F6";
    case NSF7FunctionKey: return "F7";
    case NSF8FunctionKey: return "F8";
    case NSF9FunctionKey: return "F9";
    case NSF10FunctionKey: return "F10";
    case NSF11FunctionKey: return "F11";
    case NSF12FunctionKey: return "F12";
    case 27: return "Escape";
    case 13:
    case 3: return "Enter";
    case 9: return "Tab";
    case 127: return "Backspace";
    case 32: return " ";
    default:
      if (c >= 0x20 && c < 0x7F) return std::string(1, static_cast<char>(c));
      return [chars UTF8String] ?: "Unidentified";
  }
}

std::string NSEventKeyToCode(unsigned short keyCode) {
  switch (keyCode) {
    case 0: return "KeyA";
    case 1: return "KeyS";
    case 2: return "KeyD";
    case 3: return "KeyF";
    case 4: return "KeyH";
    case 5: return "KeyG";
    case 6: return "KeyZ";
    case 7: return "KeyX";
    case 8: return "KeyC";
    case 9: return "KeyV";
    case 11: return "KeyB";
    case 12: return "KeyQ";
    case 13: return "KeyW";
    case 14: return "KeyE";
    case 15: return "KeyR";
    case 16: return "KeyY";
    case 17: return "KeyT";
    case 18: return "Digit1";
    case 19: return "Digit2";
    case 20: return "Digit3";
    case 21: return "Digit4";
    case 22: return "Digit6";
    case 23: return "Digit5";
    case 24: return "Equal";
    case 25: return "Digit9";
    case 26: return "Digit7";
    case 27: return "Minus";
    case 28: return "Digit8";
    case 29: return "Digit0";
    case 30: return "BracketRight";
    case 31: return "KeyO";
    case 32: return "KeyU";
    case 33: return "BracketLeft";
    case 34: return "KeyI";
    case 35: return "KeyP";
    case 36: return "Enter";
    case 37: return "KeyL";
    case 38: return "KeyJ";
    case 39: return "Quote";
    case 40: return "KeyK";
    case 41: return "Semicolon";
    case 42: return "Backslash";
    case 43: return "Comma";
    case 44: return "Slash";
    case 45: return "KeyN";
    case 46: return "KeyM";
    case 47: return "Period";
    case 48: return "Tab";
    case 49: return "Space";
    case 50: return "Backquote";
    case 51: return "Backspace";
    case 53: return "Escape";
    case 55: return "MetaLeft";
    case 56: return "ShiftLeft";
    case 57: return "CapsLock";
    case 58: return "AltLeft";
    case 59: return "ControlLeft";
    case 60: return "ShiftRight";
    case 61: return "AltRight";
    case 62: return "ControlRight";
    case 96: return "F5";
    case 97: return "F6";
    case 98: return "F7";
    case 99: return "F3";
    case 100: return "F8";
    case 101: return "F9";
    case 109: return "F10";
    case 103: return "F11";
    case 111: return "F12";
    case 118: return "F4";
    case 120: return "F2";
    case 122: return "F1";
    case 123: return "ArrowLeft";
    case 124: return "ArrowRight";
    case 125: return "ArrowDown";
    case 126: return "ArrowUp";
    case 117: return "Delete";
    case 114: return "Insert";
    case 115: return "Home";
    case 119: return "End";
    case 116: return "PageUp";
    case 121: return "PageDown";
    default: return "Unidentified";
  }
}

}  // namespace laufey_common
