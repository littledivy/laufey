# C ABI

laufey is built around a single C header,
[`capi/include/laufey.h`](https://github.com/littledivy/laufey/blob/main/capi/include/laufey.h).
It defines the boundary between a **backend** (a native executable embedding a
browser engine) and a **runtime** (a shared library holding the application
logic). The backend implements the ABI; the runtime consumes it.

`LAUFEY_API_VERSION` (currently `25`) versions the contract. The `version` field
on the API table lets a runtime detect the backend's vintage and avoid calling
function pointers a backend predates (older backends leave new pointers `NULL`).

## Runtime entry points

A runtime is a `.dylib`/`.so`/`.dll` that exports three symbols:

| Symbol (`*_SYMBOL` macro) | Signature                              | Role                                                                           |
| ------------------------- | -------------------------------------- | ------------------------------------------------------------------------------ |
| `laufey_runtime_init`     | `int(const laufey_backend_api_t* api)` | Backend hands the runtime the API table. Stash it; return 0 on success.        |
| `laufey_runtime_start`    | `int(void)`                            | Run application setup (create windows, register handlers). Returns when ready. |
| `laufey_runtime_shutdown` | `void(void)`                           | Tear down before the process exits.                                            |

The backend `dlopen`s the runtime, resolves these symbols, calls `init` then
`start`, and drives the OS event loop. Control flows backend → runtime through
the API table, and runtime → backend through the registered callbacks.

## The API table

`laufey_backend_api_t` is a struct of function pointers plus two data fields:

```c
struct laufey_backend_api {
  uint32_t version;     // == LAUFEY_API_VERSION the backend was built against
  void*    backend_data; // opaque; pass back as the first arg of every call
  /* ... function pointers ... */
};
```

Every function takes `backend_data` as its first argument, so the table is a
hand-rolled vtable with no global state. Windows are referenced by an opaque
`uint32_t window_id` returned from `create_window`.

The pointers group into:

- **Window lifecycle** — `create_window`, `create_window_ex` (style flags, see
  `LAUFEY_WINDOW_FLAG_*`), `close_window`, `navigate`, `set_title`,
  size/position get+set, `set_resizable`/`is_resizable`,
  `set_always_on_top`/`is_always_on_top`, `show`/`hide`/`is_visible`, `focus`,
  `quit`, `post_ui_task`.
- **Value marshalling** — the `value_*` family (below).
- **JavaScript interop** — `set_js_call_handler`, `js_call_respond`,
  `invoke_js_callback`, `release_js_callback`, `execute_js`, `set_js_namespace`,
  `poll_js_calls`, `set_js_call_notify`.
- **Event handlers** — `set_keyboard_event_handler`, `set_mouse_click_handler`,
  `set_mouse_move_handler`, `set_wheel_handler`,
  `set_cursor_enter_leave_handler`, `set_focused_handler`, `set_resize_handler`,
  `set_move_handler`, `set_close_requested_handler`.
- **Window handles** — `get_window_handle`, `get_display_handle`,
  `get_window_handle_type` (for GPU surface creation).
- **Menus** — `set_application_menu`, `show_context_menu`, `open_devtools`.
- **Dialogs** — `show_dialog`, `string_free`.
- **Dock / taskbar** — `set_dock_badge`, `bounce_dock`, `set_dock_menu`,
  `set_dock_visible`, `set_dock_reopen_handler`.
- **Tray** — `create_tray_icon`, `destroy_tray_icon`, `set_tray_icon`(`_dark`),
  `set_tray_tooltip`, `set_tray_menu`, click handlers, `get_tray_icon_bounds`.
- **Notifications** — `show_notification`, `close_notification`.
- **Permissions** — `query_permission`, `request_permission`.

See [the feature pages](window-management.md) for behavior and per-platform
differences.

## Values (`laufey_value_t`)

`laufey_value_t` is an opaque, dynamically-typed value used for everything
crossing the JS ↔ native boundary (call arguments, results, menu templates,
notification options). It models the JSON types plus binary blobs and
JS-callback handles:

- **Inspect:** `value_is_null` / `_bool` / `_int` / `_double` / `_string` /
  `_list` / `_dict` / `_binary` / `_callback`.
- **Read:** `value_get_bool` / `_int` / `_double`; `value_get_string` (returns a
  heap buffer freed with `value_free_string`); list access (`value_list_size`,
  `value_list_get`); dict access (`value_dict_get`, `value_dict_has`,
  `value_dict_size`, `value_dict_keys` + `value_free_keys`); `value_get_binary`;
  `value_get_callback_id`.
- **Build:** `value_null` / `_bool` / `_int` / `_double` / `_string` / `_list` /
  `_dict` / `_binary` (constructors take `backend_data`), then
  `value_list_append` / `_set`, `value_dict_set`.
- **Free:** `value_free`.

**Ownership.** Constructors return a value the caller owns and must `value_free`
(unless handed off). Functions that accept a template — `set_application_menu`,
`show_context_menu`, `set_tray_menu`, `set_dock_menu`, `show_notification` —
take ownership of the passed value and free it themselves.

A `_callback` value wraps a JS function passed as an argument: read its
`value_get_callback_id`, then call it later with `invoke_js_callback(id, args)`
and free it with `release_js_callback(id)`.

## JavaScript call flow

1. The runtime exposes a namespace in the page (`set_js_namespace`, default
   `"Laufey"`) and registers `set_js_call_handler`.
2. Page JS calls `Laufey.someMethod(args…)`; the backend invokes the handler
   with a `call_id`, the method name, and the arguments as a `laufey_value_t`
   list.
3. The runtime does its work and replies with
   `js_call_respond(call_id, result,
   error)` — resolving or rejecting the
   JS-side promise.

`execute_js` runs a script in a window and delivers its result/error through a
`laufey_js_result_fn`. When the runtime services calls off the UI thread, the
backend signals readiness via `set_js_call_notify` and the runtime drains the
queue with `poll_js_calls`.

## Threading

All API calls must happen on the UI thread the backend's event loop runs on.
`post_ui_task` hops onto it from another thread. `show_dialog` blocks on the UI
thread but pumps OS events so other windows stay responsive.
