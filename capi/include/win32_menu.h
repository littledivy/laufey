// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
// Win32 application menu built from laufey_value_t menu templates.

#ifndef LAUFEY_WIN32_MENU_H_
#define LAUFEY_WIN32_MENU_H_

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <laufey.h>

namespace win32_menu {

// Per-window menu state — maps command IDs to item string IDs.
struct MenuState {
  std::map<UINT, std::string> command_to_id;
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

// Recursively build an HMENU from a laufey_value_t list.
inline HMENU BuildMenuFromValue(laufey_value_t* val, const laufey_backend_api_t* api,
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

    AppendMenuA(menu, flags, cmdId, label.c_str());
    FreeVal(api, itemVal);
  }

  return menu;
}

// Set the application menu on a given HWND.
// Call this from the UI thread.
inline void SetApplicationMenu(HWND hwnd, laufey_value_t* menu_template,
                               const laufey_backend_api_t* api,
                               laufey_menu_click_fn on_click, void* on_click_data,
                               uint32_t window_id = 0) {
  if (!menu_template || !hwnd)
    return;

  MenuState& state = GetMenuStates()[hwnd];
  state.command_to_id.clear();
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
inline void ShowContextMenu(HWND hwnd, int x, int y, laufey_value_t* menu_template,
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
}

}  // namespace win32_menu

#endif  // LAUFEY_WIN32_MENU_H_
