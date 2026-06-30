// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
// Win32 application menu built from laufey_value_t menu templates.

#ifndef LAUFEY_WIN32_MENU_H_
#define LAUFEY_WIN32_MENU_H_

#include <windows.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <laufey.h>

namespace win32_menu {

// Per-window menu state — maps command IDs to item string IDs.
struct MenuState {
  std::map<UINT, std::string> command_to_id;
  // GDI bitmaps for item icons (hbmpItem). Owned here so they outlive the menu
  // and are freed when the menu is rebuilt/destroyed.
  std::vector<HBITMAP> bitmaps;
  laufey_menu_click_fn on_click = nullptr;
  void* on_click_data = nullptr;
  uint32_t window_id = 0;
  UINT next_command_id = 0x8000;  // Start above standard IDs

  UINT AllocCommandId(const std::string& item_id) {
    UINT id = next_command_id++;
    command_to_id[id] = item_id;
    return id;
  }

  void HandleCommand(UINT cmd) {
    if (on_click) {
      auto it = command_to_id.find(cmd);
      if (it != command_to_id.end()) {
        on_click(on_click_data, window_id, it->second.c_str());
      }
    }
  }
};

inline std::map<HWND, MenuState>& GetMenuStates() {
  static std::map<HWND, MenuState> states;
  return states;
}

// Parse accelerator string like "ctrl+shift+n" into display text.
inline std::string FormatAccelerator(const std::string& accel) {
  std::string result;
  std::string lower = accel;
  for (auto& c : lower)
    c = static_cast<char>(tolower(c));

  size_t pos = 0;
  std::vector<std::string> parts;
  std::string remaining = lower;
  while ((pos = remaining.find('+')) != std::string::npos) {
    parts.push_back(remaining.substr(0, pos));
    remaining = remaining.substr(pos + 1);
  }
  if (!remaining.empty())
    parts.push_back(remaining);

  for (const auto& part : parts) {
    if (!result.empty())
      result += "+";
    if (part == "cmd" || part == "command" || part == "cmdorctrl" ||
        part == "commandorcontrol") {
      result += "Ctrl";
    } else if (part == "shift") {
      result += "Shift";
    } else if (part == "alt" || part == "option") {
      result += "Alt";
    } else if (part == "ctrl" || part == "control") {
      result += "Ctrl";
    } else {
      // Capitalize the key
      std::string key = part;
      if (!key.empty())
        key[0] = static_cast<char>(toupper(key[0]));
      result += key;
    }
  }
  return result;
}

// Create a role-based menu item (standard operations).
inline bool CreateRoleMenuItem(HMENU menu, const std::string& role,
                               MenuState& state) {
  struct RoleEntry {
    const char* role;
    const char* label;
  };
  static const RoleEntry roles[] = {
      {"quit", "E&xit"},
      {"copy", "&Copy"},
      {"paste", "&Paste"},
      {"cut", "Cu&t"},
      {"selectall", "&Select All"},
      {"selectAll", "&Select All"},
      {"undo", "&Undo"},
      {"redo", "&Redo"},
      {"minimize", "Mi&nimize"},
      {"close", "&Close"},
      {"about", "&About"},
  };

  for (const auto& entry : roles) {
    if (role == entry.role) {
      UINT id = state.AllocCommandId(role);
      AppendMenuA(menu, MF_STRING, id, entry.label);
      return true;
    }
  }
  return false;
}

// Helper to free a laufey_value_t* if non-null.
inline void FreeVal(const laufey_backend_api_t* api, laufey_value_t* v) {
  if (v)
    api->value_free(v);
}

// Decode a PNG file into a 32bpp premultiplied-alpha top-down DIB section
// suitable for a menu item bitmap (hbmpItem). Scaled to the small-icon metric
// so it lines up with the menu gutter. Returns nullptr on any failure; the
// caller owns the returned HBITMAP and must DeleteObject it. Unlike macOS,
// the image is rendered as-is — there is no template tinting on selection.
inline HBITMAP LoadMenuIconBitmap(const std::string& path) {
  if (path.empty())
    return nullptr;

  int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (wlen <= 0)
    return nullptr;
  std::wstring wpath(static_cast<size_t>(wlen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }

  IWICBitmapDecoder* decoder = nullptr;
  factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                     WICDecodeMetadataCacheOnLoad, &decoder);
  IWICBitmapFrameDecode* frame = nullptr;
  if (decoder)
    decoder->GetFrame(0, &frame);
  IWICFormatConverter* conv = nullptr;
  factory->CreateFormatConverter(&conv);
  if (frame && conv) {
    conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                     WICBitmapDitherTypeNone, nullptr, 0.0,
                     WICBitmapPaletteTypeCustom);
  }

  int metric = GetSystemMetrics(SM_CXSMICON);
  UINT side = metric > 0 ? static_cast<UINT>(metric) : 16;
  IWICBitmapScaler* scaler = nullptr;
  factory->CreateBitmapScaler(&scaler);
  if (conv && scaler) {
    scaler->Initialize(conv, side, side,
                       WICBitmapInterpolationModeHighQualityCubic);
  }

  HBITMAP hbmp = nullptr;
  if (scaler) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(side);
    bi.bmiHeader.biHeight = -static_cast<LONG>(side);  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    hbmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    UINT stride = side * 4;
    if (hbmp && bits &&
        FAILED(scaler->CopyPixels(nullptr, stride, stride * side,
                                  static_cast<BYTE*>(bits)))) {
      DeleteObject(hbmp);
      hbmp = nullptr;
    } else if (hbmp && !bits) {
      DeleteObject(hbmp);
      hbmp = nullptr;
    }
  }

  if (scaler)
    scaler->Release();
  if (conv)
    conv->Release();
  if (frame)
    frame->Release();
  if (decoder)
    decoder->Release();
  factory->Release();
  return hbmp;
}

// Recursively build an HMENU from a laufey_value_t list.
inline HMENU BuildMenuFromValue(laufey_value_t* val,
                                const laufey_backend_api_t* api,
                                MenuState& state) {
  if (!val || !api->value_is_list(val))
    return nullptr;

  HMENU menu = CreateMenu();
  size_t count = api->value_list_size(val);

  for (size_t i = 0; i < count; ++i) {
    laufey_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal)) {
      FreeVal(api, itemVal);
      continue;
    }

    // Check for separator
    laufey_value_t* typeVal = api->value_dict_get(itemVal, "type");
    if (typeVal && api->value_is_string(typeVal)) {
      size_t len = 0;
      char* typeStr = api->value_get_string(typeVal, &len);
      FreeVal(api, typeVal);
      if (typeStr && std::string(typeStr) == "separator") {
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
        api->value_free_string(typeStr);
        FreeVal(api, itemVal);
        continue;
      }
      if (typeStr)
        api->value_free_string(typeStr);
    }

    // Check for role
    laufey_value_t* roleVal = api->value_dict_get(itemVal, "role");
    if (roleVal && api->value_is_string(roleVal)) {
      size_t len = 0;
      char* roleStr = api->value_get_string(roleVal, &len);
      FreeVal(api, roleVal);
      if (roleStr) {
        CreateRoleMenuItem(menu, roleStr, state);
        api->value_free_string(roleStr);
        FreeVal(api, itemVal);
        continue;
      }
    }

    // Regular item or submenu — needs a label
    laufey_value_t* labelVal = api->value_dict_get(itemVal, "label");
    if (!labelVal || !api->value_is_string(labelVal)) {
      FreeVal(api, labelVal);
      FreeVal(api, itemVal);
      continue;
    }

    size_t labelLen = 0;
    char* labelStr = api->value_get_string(labelVal, &labelLen);
    FreeVal(api, labelVal);
    if (!labelStr) {
      FreeVal(api, itemVal);
      continue;
    }
    std::string label = labelStr;
    api->value_free_string(labelStr);

    // Append accelerator text
    laufey_value_t* accelVal = api->value_dict_get(itemVal, "accelerator");
    if (accelVal && api->value_is_string(accelVal)) {
      size_t accelLen = 0;
      char* accelStr = api->value_get_string(accelVal, &accelLen);
      if (accelStr) {
        label += "\t" + FormatAccelerator(accelStr);
        api->value_free_string(accelStr);
      }
    }
    FreeVal(api, accelVal);

    // Check for submenu
    laufey_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      HMENU submenu = BuildMenuFromValue(submenuVal, api, state);
      FreeVal(api, submenuVal);
      if (submenu) {
        AppendMenuA(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu),
                    label.c_str());
      }
      FreeVal(api, itemVal);
      continue;
    }
    FreeVal(api, submenuVal);

    // Regular clickable item
    std::string itemId;
    laufey_value_t* idVal = api->value_dict_get(itemVal, "id");
    if (idVal && api->value_is_string(idVal)) {
      size_t idLen = 0;
      char* idStr = api->value_get_string(idVal, &idLen);
      if (idStr) {
        itemId = idStr;
        api->value_free_string(idStr);
      }
    }
    FreeVal(api, idVal);

    UINT cmdId = state.AllocCommandId(itemId.empty() ? label : itemId);

    UINT flags = MF_STRING;
    laufey_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal) &&
        !api->value_get_bool(enabledVal)) {
      flags |= MF_GRAYED;
    }
    FreeVal(api, enabledVal);

    // checked -> MF_CHECKED checkmark.
    laufey_value_t* checkedVal = api->value_dict_get(itemVal, "checked");
    if (checkedVal && api->value_is_bool(checkedVal) &&
        api->value_get_bool(checkedVal)) {
      flags |= MF_CHECKED;
    }
    FreeVal(api, checkedVal);

    // icon -> menu item bitmap (a PNG file path), loaded below after the item
    // exists so it can be attached by command id.
    std::string iconPath;
    laufey_value_t* iconVal = api->value_dict_get(itemVal, "icon");
    if (iconVal && api->value_is_string(iconVal)) {
      size_t iconLen = 0;
      char* iconStr = api->value_get_string(iconVal, &iconLen);
      if (iconStr) {
        iconPath = iconStr;
        api->value_free_string(iconStr);
      }
    }
    FreeVal(api, iconVal);

    AppendMenuA(menu, flags, cmdId, label.c_str());

    if (!iconPath.empty()) {
      HBITMAP hbmp = LoadMenuIconBitmap(iconPath);
      if (hbmp) {
        MENUITEMINFOA mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_BITMAP;
        mii.hbmpItem = hbmp;
        SetMenuItemInfoA(menu, cmdId, FALSE, &mii);
        state.bitmaps.push_back(hbmp);
      }
    }

    FreeVal(api, itemVal);
  }

  return menu;
}

// Set the application menu on a given HWND.
// Call this from the UI thread.
inline void SetApplicationMenu(HWND hwnd, laufey_value_t* menu_template,
                               const laufey_backend_api_t* api,
                               laufey_menu_click_fn on_click,
                               void* on_click_data, uint32_t window_id = 0) {
  if (!menu_template || !hwnd)
    return;

  MenuState& state = GetMenuStates()[hwnd];
  state.command_to_id.clear();
  // The previously built menu still references its bitmaps until DestroyMenu
  // below, so hold them aside and free them only after the old menu is gone.
  std::vector<HBITMAP> oldBitmaps = std::move(state.bitmaps);
  state.bitmaps.clear();
  state.next_command_id = 0x8000;
  state.on_click = on_click;
  state.on_click_data = on_click_data;
  state.window_id = window_id;

  // Destroy the old menu to avoid HMENU leak
  HMENU oldMenu = GetMenu(hwnd);

  HMENU menubar = BuildMenuFromValue(menu_template, api, state);
  if (menubar) {
    SetMenu(hwnd, menubar);
    DrawMenuBar(hwnd);
  }

  if (oldMenu) {
    DestroyMenu(oldMenu);
  }
  for (HBITMAP bmp : oldBitmaps) {
    if (bmp)
      DeleteObject(bmp);
  }
}

// Call this from WndProc on WM_COMMAND to dispatch menu clicks.
inline bool HandleMenuCommand(HWND hwnd, WPARAM wParam) {
  UINT cmd = LOWORD(wParam);
  auto& states = GetMenuStates();
  auto it = states.find(hwnd);
  if (it == states.end())
    return false;
  MenuState& state = it->second;
  auto cmd_it = state.command_to_id.find(cmd);
  if (cmd_it != state.command_to_id.end()) {
    state.HandleCommand(cmd);
    return true;
  }
  return false;
}

// Show a context menu at the given position (client coordinates).
// The menu is built from the same laufey_value_t template as application menus.
inline void ShowContextMenu(HWND hwnd, int x, int y,
                            laufey_value_t* menu_template,
                            const laufey_backend_api_t* api,
                            laufey_menu_click_fn on_click, void* on_click_data,
                            uint32_t window_id = 0) {
  if (!menu_template || !hwnd)
    return;

  MenuState state;
  state.on_click = on_click;
  state.on_click_data = on_click_data;
  state.window_id = window_id;

  HMENU popup = BuildMenuFromValue(menu_template, api, state);
  if (!popup)
    return;

  // Convert client coordinates to screen coordinates
  POINT pt = {x, y};
  ClientToScreen(hwnd, &pt);

  // TrackPopupMenu blocks until the user selects an item or dismisses.
  // TPM_RETURNCMD makes it return the selected command ID directly.
  UINT cmd = static_cast<UINT>(TrackPopupMenu(
      popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr));

  if (cmd != 0) {
    state.HandleCommand(cmd);
  }

  DestroyMenu(popup);
  for (HBITMAP bmp : state.bitmaps) {
    if (bmp)
      DeleteObject(bmp);
  }
}

}  // namespace win32_menu

#endif  // LAUFEY_WIN32_MENU_H_
