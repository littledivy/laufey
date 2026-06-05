# Window handles (GPU surfaces)

When you want to draw a window's contents yourself — with a GPU API such as
wgpu, Vulkan, or Metal — rather than load web content, laufey gives you the raw
operating-system handles for the window. This is the primary reason the Winit
backend exists.

```rust
let win = Window::new(800, 600);

let handle = win.get_window_handle();   // NSView*, HWND, X11 Window, or wl_surface*
let display = win.get_display_handle(); // X11 Display* or wl_display* (null elsewhere)
match win.get_window_handle_type() {
  // One of the LAUFEY_WINDOW_HANDLE_* constants: AppKit, Win32, X11, or Wayland.
  handle_type => { /* create a rendering surface for this platform */ }
}
```

The window handle, the display handle, and the type constant together provide
everything a library such as `raw-window-handle` needs to build a rendering
surface. The CEF and WebView backends own and render into their windows
themselves, so they do not expose these handles.
