// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// GDK keyval / hardware keycode → W3C key/code mapping. Used only by
// the webview Linux backend (CEF normalizes to Windows VK and uses
// keymap_vk.cc).

#include "laufey_backend_common.h"

#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>

#include <string>

namespace laufey_common {

std::string GdkKeyvalToKey(unsigned int keyval) {
  switch (keyval) {
    case GDK_KEY_BackSpace: return "Backspace";
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: return "Tab";
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter: return "Enter";
    case GDK_KEY_Escape: return "Escape";
    case GDK_KEY_space: return " ";
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete: return "Delete";
    case GDK_KEY_Insert:
    case GDK_KEY_KP_Insert: return "Insert";
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home: return "Home";
    case GDK_KEY_End:
    case GDK_KEY_KP_End: return "End";
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up: return "PageUp";
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down: return "PageDown";
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left: return "ArrowLeft";
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right: return "ArrowRight";
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up: return "ArrowUp";
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down: return "ArrowDown";
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R: return "Shift";
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R: return "Control";
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R: return "Alt";
    case GDK_KEY_Meta_L:
    case GDK_KEY_Meta_R:
    case GDK_KEY_Super_L:
    case GDK_KEY_Super_R: return "Meta";
    case GDK_KEY_Caps_Lock: return "CapsLock";
    case GDK_KEY_Num_Lock: return "NumLock";
    case GDK_KEY_Scroll_Lock: return "ScrollLock";
    case GDK_KEY_F1: return "F1";
    case GDK_KEY_F2: return "F2";
    case GDK_KEY_F3: return "F3";
    case GDK_KEY_F4: return "F4";
    case GDK_KEY_F5: return "F5";
    case GDK_KEY_F6: return "F6";
    case GDK_KEY_F7: return "F7";
    case GDK_KEY_F8: return "F8";
    case GDK_KEY_F9: return "F9";
    case GDK_KEY_F10: return "F10";
    case GDK_KEY_F11: return "F11";
    case GDK_KEY_F12: return "F12";
    case GDK_KEY_Pause: return "Pause";
    default: {
      guint32 uc = gdk_keyval_to_unicode(keyval);
      if (uc > 0 && g_unichar_isprint(uc)) {
        char buf[7];
        int len = g_unichar_to_utf8(uc, buf);
        buf[len] = '\0';
        return std::string(buf);
      }
      return "Unidentified";
    }
  }
}

std::string GdkKeycodeToCode(unsigned int hardware_keycode) {
  switch (hardware_keycode) {
    case 9: return "Escape";
    case 10: return "Digit1";
    case 11: return "Digit2";
    case 12: return "Digit3";
    case 13: return "Digit4";
    case 14: return "Digit5";
    case 15: return "Digit6";
    case 16: return "Digit7";
    case 17: return "Digit8";
    case 18: return "Digit9";
    case 19: return "Digit0";
    case 20: return "Minus";
    case 21: return "Equal";
    case 22: return "Backspace";
    case 23: return "Tab";
    case 24: return "KeyQ";
    case 25: return "KeyW";
    case 26: return "KeyE";
    case 27: return "KeyR";
    case 28: return "KeyT";
    case 29: return "KeyY";
    case 30: return "KeyU";
    case 31: return "KeyI";
    case 32: return "KeyO";
    case 33: return "KeyP";
    case 34: return "BracketLeft";
    case 35: return "BracketRight";
    case 36: return "Enter";
    case 37: return "ControlLeft";
    case 38: return "KeyA";
    case 39: return "KeyS";
    case 40: return "KeyD";
    case 41: return "KeyF";
    case 42: return "KeyG";
    case 43: return "KeyH";
    case 44: return "KeyJ";
    case 45: return "KeyK";
    case 46: return "KeyL";
    case 47: return "Semicolon";
    case 48: return "Quote";
    case 49: return "Backquote";
    case 50: return "ShiftLeft";
    case 51: return "Backslash";
    case 52: return "KeyZ";
    case 53: return "KeyX";
    case 54: return "KeyC";
    case 55: return "KeyV";
    case 56: return "KeyB";
    case 57: return "KeyN";
    case 58: return "KeyM";
    case 59: return "Comma";
    case 60: return "Period";
    case 61: return "Slash";
    case 62: return "ShiftRight";
    case 64: return "AltLeft";
    case 65: return "Space";
    case 66: return "CapsLock";
    case 67: return "F1";
    case 68: return "F2";
    case 69: return "F3";
    case 70: return "F4";
    case 71: return "F5";
    case 72: return "F6";
    case 73: return "F7";
    case 74: return "F8";
    case 75: return "F9";
    case 76: return "F10";
    case 95: return "F11";
    case 96: return "F12";
    case 105: return "ControlRight";
    case 108: return "AltRight";
    case 110: return "Home";
    case 111: return "ArrowUp";
    case 112: return "PageUp";
    case 113: return "ArrowLeft";
    case 114: return "ArrowRight";
    case 115: return "End";
    case 116: return "ArrowDown";
    case 117: return "PageDown";
    case 118: return "Insert";
    case 119: return "Delete";
    case 133: return "MetaLeft";
    case 134: return "MetaRight";
    default: return "Unidentified";
  }
}

}  // namespace laufey_common
