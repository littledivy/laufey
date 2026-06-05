// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <map>

#include "include/cef_app.h"
#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "app.h"
#include "renderer_app.h"
#include "runtime_loader.h"

// --- Native event monitors (Linux / X11) ---
// Uses XI2 (X Input Extension 2) on a dedicated X11 connection to monitor
// mouse, scroll, cursor enter/leave, and focus events for CEF Views windows.
// Window resize/move events use StructureNotifyMask on the same connection.
//
// A separate X11 connection is used so that event selection doesn't interfere
// with CEF's or GDK's own event handling. The connection's FD is integrated
// into the GLib main loop via g_io_add_watch.

#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>

static Display* g_monitor_display = nullptr;
static int g_xi2_opcode = 0;
static GIOChannel* g_io_channel = nullptr;
static guint g_io_source_id = 0;
static bool g_monitor_installed = false;

// Cache: WM frame XID -> (laufey_id, CEF content XID).
// With a reparenting WM, the top-level child of root is the WM frame,
// not the CEF window. This cache maps frame XIDs to the laufey IDs and
// the actual content window for coordinate translation.
struct CachedWindow {
  uint32_t laufey_id;
  Window content_xid;
};
static std::map<Window, CachedWindow> g_frame_cache;

static uint32_t XI2ModsToLaufey(XIModifierState* mods) {
  uint32_t laufey = 0;
  if (mods->effective & ShiftMask)
    laufey |= LAUFEY_MOD_SHIFT;
  if (mods->effective & ControlMask)
    laufey |= LAUFEY_MOD_CONTROL;
  if (mods->effective & Mod1Mask)
    laufey |= LAUFEY_MOD_ALT;
  if (mods->effective & Mod4Mask)
    laufey |= LAUFEY_MOD_META;
  return laufey;
}

// Multi-click detection: same button within LAUFEY_MULTI_CLICK_TIME_MS and
// LAUFEY_MULTI_CLICK_DISTANCE_PX of the previous click increments the count.
static constexpr int LAUFEY_MULTI_CLICK_TIME_MS = 500;
static constexpr int LAUFEY_MULTI_CLICK_DISTANCE_PX = 5;

struct ClickTracker {
  Time last_time = 0;
  int last_button = 0;
  double last_x = 0;
  double last_y = 0;
  int count = 0;
};
static ClickTracker g_click_tracker;

// Tracks which laufey window the cursor is currently over and last cursor
// position within it. Populated from per-window Enter/Motion events.
// RawButtonPress/Release events lack window context, so we route them to
// this tracked window with these coordinates.
static uint32_t g_hover_wid = 0;
static double g_hover_x = 0;
static double g_hover_y = 0;
static uint32_t g_hover_modifiers = 0;

static int ComputeClickCount(Time time, int button, double x, double y) {
  double dx = x - g_click_tracker.last_x;
  double dy = y - g_click_tracker.last_y;
  bool close = (dx * dx + dy * dy) <=
               (LAUFEY_MULTI_CLICK_DISTANCE_PX * LAUFEY_MULTI_CLICK_DISTANCE_PX);
  if (button == g_click_tracker.last_button &&
      time - g_click_tracker.last_time <= (Time)LAUFEY_MULTI_CLICK_TIME_MS &&
      close) {
    g_click_tracker.count += 1;
  } else {
    g_click_tracker.count = 1;
  }
  g_click_tracker.last_time = time;
  g_click_tracker.last_button = button;
  g_click_tracker.last_x = x;
  g_click_tracker.last_y = y;
  return g_click_tracker.count;
}

static int XI2ButtonToLaufey(int detail) {
  switch (detail) {
    case 1:
      return LAUFEY_MOUSE_BUTTON_LEFT;
    case 2:
      return LAUFEY_MOUSE_BUTTON_MIDDLE;
    case 3:
      return LAUFEY_MOUSE_BUTTON_RIGHT;
    case 8:
      return LAUFEY_MOUSE_BUTTON_BACK;
    case 9:
      return LAUFEY_MOUSE_BUTTON_FORWARD;
    default:
      return LAUFEY_MOUSE_BUTTON_LEFT;
  }
}

// Resolve an XI2 device event to a laufey window ID and content-relative coords.
// With per-window XI2 selection, dev->event is the window we selected on
// (the CEF content window) and dev->event_x/y are already window-relative.
static bool ResolveWindow(XIDeviceEvent* dev, uint32_t* out_wid, double* out_x,
                          double* out_y) {
  if (!dev->event)
    return false;

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  uint32_t wid = loader->GetLaufeyIdForNativeHandle((void*)(uintptr_t)dev->event);

  if (wid == 0) {
    // Fall back to the frame-cache mapping in case selection landed on a
    // reparented ancestor.
    auto it = g_frame_cache.find(dev->event);
    if (it != g_frame_cache.end())
      wid = it->second.laufey_id;
  }

  if (wid == 0)
    return false;

  *out_wid = wid;
  *out_x = dev->event_x;
  *out_y = dev->event_y;
  return true;
}

// Resolve a window XID from an XI2 enter/focus event to a laufey ID.
static uint32_t ResolveLaufeyId(Window xid) {
  if (!xid)
    return 0;
  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  uint32_t wid = loader->GetLaufeyIdForNativeHandle((void*)(uintptr_t)xid);
  if (wid == 0) {
    auto it = g_frame_cache.find(xid);
    if (it != g_frame_cache.end())
      wid = it->second.laufey_id;
  }
  return wid;
}

static void ProcessXI2Event(XEvent* xev) {
  if (!XGetEventData(xev->xcookie.display, &xev->xcookie))
    return;

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  int evtype = xev->xcookie.evtype;

  if (getenv("LAUFEY_CLICK_DEBUG")) {
    fprintf(stderr, "[laufey-click] xi2 evtype=%d\n", evtype);
  }

  if (evtype == XI_Enter || evtype == XI_Leave) {
    // Filter out synthetic Enter/Leave events generated by grab transitions
    // (CEF acquires/releases pointer grabs on button press/release). Only
    // real cursor crossings have mode=XINotifyNormal.
    XIEnterEvent* enter = static_cast<XIEnterEvent*>(xev->xcookie.data);
    if (enter->mode != XINotifyNormal) {
      XFreeEventData(xev->xcookie.display, &xev->xcookie);
      return;
    }
  }

  if (evtype == XI_ButtonPress || evtype == XI_ButtonRelease ||
      evtype == XI_Motion || evtype == XI_Enter || evtype == XI_Leave) {
    XIDeviceEvent* dev = static_cast<XIDeviceEvent*>(xev->xcookie.data);
    uint32_t wid;
    double x, y;

    if (ResolveWindow(dev, &wid, &x, &y)) {
      uint32_t modifiers = XI2ModsToLaufey(&dev->mods);

      switch (evtype) {
        case XI_ButtonPress: {
          int detail = dev->detail;
          if (detail >= 4 && detail <= 7) {
            double dx = 0, dy = 0;
            if (detail == 4)
              dy = -1.0;
            else if (detail == 5)
              dy = 1.0;
            else if (detail == 6)
              dx = -1.0;
            else if (detail == 7)
              dx = 1.0;
            loader->DispatchWheelEvent(wid, dx, dy, x, y, modifiers,
                                       LAUFEY_WHEEL_DELTA_LINE);
          } else {
            int laufey_button = XI2ButtonToLaufey(detail);
            int click_count = ComputeClickCount(dev->time, laufey_button, x, y);
            loader->DispatchMouseClickEvent(wid, LAUFEY_MOUSE_PRESSED, laufey_button,
                                            x, y, modifiers, click_count);
            // CEF holds an XI2 grab during press/release on its own X
            // connection, so XI_ButtonRelease is never delivered to our
            // monitor (and raw XI2 release events are unreliable under
            // XWayland). Synthesize a release immediately so click/dblclick
            // dispatch in deno still works. CEF's own press+release handling
            // (for its internal rendering / drag) is unaffected because it
            // runs on a separate X connection.
            loader->DispatchMouseClickEvent(wid, LAUFEY_MOUSE_RELEASED, laufey_button,
                                            x, y, modifiers, click_count);
          }
          break;
        }
        case XI_ButtonRelease: {
          int detail = dev->detail;
          if (detail >= 4 && detail <= 7)
            break;
          int laufey_button = XI2ButtonToLaufey(detail);
          loader->DispatchMouseClickEvent(wid, LAUFEY_MOUSE_RELEASED, laufey_button,
                                          x, y, modifiers,
                                          g_click_tracker.count);
          break;
        }
        case XI_Motion:
          g_hover_wid = wid;
          g_hover_x = x;
          g_hover_y = y;
          g_hover_modifiers = modifiers;
          loader->DispatchMouseMoveEvent(wid, x, y, modifiers);
          break;
        case XI_Enter:
          g_hover_wid = wid;
          g_hover_x = x;
          g_hover_y = y;
          g_hover_modifiers = modifiers;
          loader->DispatchCursorEnterLeaveEvent(wid, 1, x, y, modifiers);
          break;
        case XI_Leave:
          if (g_hover_wid == wid)
            g_hover_wid = 0;
          loader->DispatchCursorEnterLeaveEvent(wid, 0, x, y, modifiers);
          break;
      }
    }
  } else if (evtype == XI_RawButtonRelease) {
    // Raw release bypasses CEF's drag grabs that swallow normal releases.
    // Route to the currently hovered window with last cursor coords.
    XIRawEvent* raw = static_cast<XIRawEvent*>(xev->xcookie.data);
    int detail = raw->detail;
    if (getenv("LAUFEY_CLICK_DEBUG")) {
      fprintf(stderr,
              "[laufey-click] raw-release detail=%d hover_wid=%u count=%d\n",
              detail, g_hover_wid, g_click_tracker.count);
    }
    if (detail < 4 || detail > 7) {
      if (g_hover_wid != 0) {
        int laufey_button = XI2ButtonToLaufey(detail);
        loader->DispatchMouseClickEvent(
            g_hover_wid, LAUFEY_MOUSE_RELEASED, laufey_button, g_hover_x, g_hover_y,
            g_hover_modifiers, g_click_tracker.count);
      }
    }
  } else if (evtype == XI_FocusIn || evtype == XI_FocusOut) {
    XIEnterEvent* enter = static_cast<XIEnterEvent*>(xev->xcookie.data);
    Window target = enter->child ? enter->child : enter->event;
    uint32_t wid = ResolveLaufeyId(target);
    if (wid > 0) {
      loader->DispatchFocusedEvent(wid, evtype == XI_FocusIn ? 1 : 0);
    }
  }

  XFreeEventData(xev->xcookie.display, &xev->xcookie);
}

static void ProcessStructureEvent(XEvent* xev) {
  if (xev->type != ConfigureNotify)
    return;

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  XConfigureEvent* config = &xev->xconfigure;

  uint32_t wid =
      loader->GetLaufeyIdForNativeHandle((void*)(uintptr_t)config->window);
  if (wid > 0) {
    loader->DispatchResizeEvent(wid, config->width, config->height);
    loader->DispatchMoveEvent(wid, config->x, config->y);
  }
}

static gboolean X11IoCallback(GIOChannel* source, GIOCondition condition,
                              gpointer data) {
  if (!g_monitor_display)
    return FALSE;

  while (XPending(g_monitor_display)) {
    XEvent xev;
    XNextEvent(g_monitor_display, &xev);

    if (xev.type == GenericEvent && xev.xcookie.extension == g_xi2_opcode) {
      ProcessXI2Event(&xev);
    } else {
      ProcessStructureEvent(&xev);
    }
  }

  return TRUE;
}

#endif  // GDK_WINDOWING_X11

void InstallNativeMouseMonitor() {
#ifdef GDK_WINDOWING_X11
  if (g_monitor_installed)
    return;

  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return;

  // Open a dedicated X11 connection for event monitoring.
  g_monitor_display = XOpenDisplay(nullptr);
  if (!g_monitor_display)
    return;

  // Check for XI2 support.
  int xi2_event, xi2_error;
  if (!XQueryExtension(g_monitor_display, "XInputExtension", &g_xi2_opcode,
                       &xi2_event, &xi2_error)) {
    XCloseDisplay(g_monitor_display);
    g_monitor_display = nullptr;
    return;
  }

  int major = 2, minor = 0;
  if (XIQueryVersion(g_monitor_display, &major, &minor) != Success) {
    XCloseDisplay(g_monitor_display);
    g_monitor_display = nullptr;
    return;
  }

  // Select XI2 RawButtonRelease on the root. Raw events bypass grabs, so
  // releases still reach us even while CEF holds a drag grab on the
  // window. Press/motion are delivered fine via per-window selection.
  Window root = DefaultRootWindow(g_monitor_display);
  unsigned char raw_mask_bits[XIMaskLen(XI_LASTEVENT)] = {};
  XISetMask(raw_mask_bits, XI_RawButtonRelease);

  XIEventMask raw_mask;
  raw_mask.deviceid = XIAllMasterDevices;
  raw_mask.mask_len = sizeof(raw_mask_bits);
  raw_mask.mask = raw_mask_bits;
  XISelectEvents(g_monitor_display, root, &raw_mask, 1);
  XFlush(g_monitor_display);

  // Integrate the monitoring connection into the GLib main loop.
  int fd = ConnectionNumber(g_monitor_display);
  g_io_channel = g_io_channel_unix_new(fd);
  g_io_source_id = g_io_add_watch(
      g_io_channel, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
      X11IoCallback, nullptr);

  g_monitor_installed = true;
#endif
}

void RemoveNativeMouseMonitor() {
#ifdef GDK_WINDOWING_X11
  if (!g_monitor_installed)
    return;

  if (g_io_source_id) {
    g_source_remove(g_io_source_id);
    g_io_source_id = 0;
  }
  if (g_io_channel) {
    g_io_channel_unref(g_io_channel);
    g_io_channel = nullptr;
  }
  if (g_monitor_display) {
    XCloseDisplay(g_monitor_display);
    g_monitor_display = nullptr;
  }

  g_frame_cache.clear();
  g_monitor_installed = false;
#endif
}

void SetLinuxWindowResizable(unsigned long xid, bool resizable) {
#ifdef GDK_WINDOWING_X11
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return;
  Display* dpy = GDK_DISPLAY_XDISPLAY(gdk_display);

  XSizeHints hints;
  long supplied;
  XGetWMNormalHints(dpy, xid, &hints, &supplied);

  if (resizable) {
    hints.flags &= ~(PMinSize | PMaxSize);
    hints.min_width = 0;
    hints.min_height = 0;
    hints.max_width = 0;
    hints.max_height = 0;
  } else {
    XWindowAttributes attrs;
    XGetWindowAttributes(dpy, xid, &attrs);
    hints.flags |= PMinSize | PMaxSize;
    hints.min_width = attrs.width;
    hints.min_height = attrs.height;
    hints.max_width = attrs.width;
    hints.max_height = attrs.height;
  }
  XSetWMNormalHints(dpy, xid, &hints);
  XFlush(dpy);
#endif
}

void ConfigureLinuxWindowAsPanel(unsigned long xid) {
#ifdef GDK_WINDOWING_X11
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return;
  Display* dpy = GDK_DISPLAY_XDISPLAY(gdk_display);

  // Mark the window as a utility/panel type so the WM treats it as an
  // auxiliary tool window — floated, not part of normal focus/taskbar
  // handling — which is the closest X11 equivalent of a non-activating
  // macOS panel.
  Atom net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
  Atom utility = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
  XChangeProperty(dpy, xid, net_wm_window_type, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&utility), 1);

  // Keep it out of the taskbar and pager, like a tray popover.
  Atom net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
  Atom skip_taskbar = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
  Atom skip_pager = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
  Atom states[] = {skip_taskbar, skip_pager};
  XChangeProperty(dpy, xid, net_wm_state, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(states), 2);

  XFlush(dpy);
#endif
}

bool IsLinuxWindowResizable(unsigned long xid) {
#ifdef GDK_WINDOWING_X11
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
    return true;
  Display* dpy = GDK_DISPLAY_XDISPLAY(gdk_display);

  XSizeHints hints;
  long supplied;
  XGetWMNormalHints(dpy, xid, &hints, &supplied);

  if ((hints.flags & PMinSize) && (hints.flags & PMaxSize)) {
    return hints.min_width != hints.max_width ||
           hints.min_height != hints.max_height;
  }
  return true;
#else
  return true;
#endif
}

void MonitorLinuxWindowEvents(unsigned long xid) {
#ifdef GDK_WINDOWING_X11
  if (!g_monitor_display)
    return;

  // Select StructureNotifyMask on the CEF window to get ConfigureNotify
  // (resize/move) events on our monitoring connection.
  XSelectInput(g_monitor_display, xid, StructureNotifyMask);

  // Select XI2 events on this CEF window for all master devices. Per-window
  // selection works reliably under both native X11 and XWayland, unlike
  // root-window passive selection. Release is handled via RawButtonRelease
  // on root so it bypasses CEF's drag grabs (see InstallNativeMouseMonitor).
  unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = {};
  XISetMask(mask_bits, XI_ButtonPress);
  XISetMask(mask_bits, XI_ButtonRelease);
  XISetMask(mask_bits, XI_Motion);
  XISetMask(mask_bits, XI_Enter);
  XISetMask(mask_bits, XI_Leave);
  XISetMask(mask_bits, XI_FocusIn);
  XISetMask(mask_bits, XI_FocusOut);

  XIEventMask xi_mask;
  xi_mask.deviceid = XIAllMasterDevices;
  xi_mask.mask_len = sizeof(mask_bits);
  xi_mask.mask = mask_bits;
  XISelectEvents(g_monitor_display, xid, &xi_mask, 1);

  // Walk up the window tree to find the WM frame (direct child of root).
  // With a reparenting WM, the CEF window is reparented inside the frame.
  // XI2 events on root report the frame as `child`, so we cache the
  // frame → (laufey_id, content_xid) mapping for fast lookup.
  Window root_ret, parent;
  Window* children;
  unsigned int nchildren;
  Window current = xid;

  while (XQueryTree(g_monitor_display, current, &root_ret, &parent, &children,
                    &nchildren)) {
    if (children)
      XFree(children);
    if (parent == root_ret || parent == 0) {
      // current is the top-level frame (or the window itself if no WM).
      if (current != xid) {
        uint32_t wid = RuntimeLoader::GetInstance()->GetLaufeyIdForNativeHandle(
            (void*)(uintptr_t)xid);
        if (wid > 0) {
          g_frame_cache[current] = {wid, xid};
        }
      }
      break;
    }
    current = parent;
  }

  XFlush(g_monitor_display);
#else
  (void)xid;
#endif
}

// --- Headless / forked worker support ---

static int run_headless(const std::string& runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  if (runtimePath.empty()) {
    std::cerr << "No runtime library found for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start headless worker runtime." << std::endl;
    return 1;
  }

  loader->Shutdown();
  return 0;
}

static bool is_forked_worker() {
  return getenv("NODE_CHANNEL_FD") != nullptr ||
         getenv("NEXT_PRIVATE_WORKER") != nullptr;
}

static bool is_cli_worker_command(int argc, char* argv[]) {
  if (argc < 3 || strcmp(argv[1], "run") != 0) {
    return false;
  }
  for (int i = 2; i < argc; ++i) {
    if (argv[i][0] == '-') {
      continue;
    }
    return true;
  }
  return false;
}

// Combined app that handles both browser and renderer processes (single-exe
// model)
class LaufeyCombinedApp : public CefApp, public CefBrowserProcessHandler {
 public:
  LaufeyCombinedApp() : renderer_app_(new LaufeyRendererApp()) {}

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return renderer_app_->GetRenderProcessHandler();
  }

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {}

  void OnContextInitialized() override {
    CEF_REQUIRE_UI_THREAD();

    // Keep the handler alive for the lifetime of the app.
    // Backend_CreateWindow uses LaufeyHandler::GetInstance() from the runtime
    // thread, so the handler must outlive this function scope.
    static CefRefPtr<LaufeyHandler> handler(new LaufeyHandler());

    if (!g_runtime_path.empty()) {
      if (!RuntimeLoader::GetInstance()->Load(g_runtime_path)) {
        std::cerr << "Failed to load runtime, exiting" << std::endl;
        CefQuitMessageLoop();
        return;
      }
      // Defer Start() to the next message loop iteration.
      // OnContextInitialized runs during CefInitialize(), before
      // CefRunMessageLoop() has started. The runtime thread's
      // Backend_CreateWindow posts CefPostTasks to the UI thread and blocks
      // until they complete — this deadlocks if the loop isn't running yet.
      CefPostTask(TID_UI, base::BindOnce(
                              []() { RuntimeLoader::GetInstance()->Start(); }));
    } else {
      // No runtime: create a default window for demo
      uint32_t laufey_id = RuntimeLoader::GetInstance()->AllocateWindowId();
      g_pending_laufey_ids.push(laufey_id);
      CefBrowserSettings browser_settings;
      CefRefPtr<CefBrowserView> browser_view =
          CefBrowserView::CreateBrowserView(handler, "https://example.com",
                                            browser_settings, nullptr, nullptr,
                                            nullptr);
      CefWindow::CreateTopLevelWindow(
          new LaufeyWindowDelegate(browser_view, laufey_id));
    }
  }

 private:
  CefRefPtr<LaufeyRendererApp> renderer_app_;
  IMPLEMENT_REFCOUNTING(LaufeyCombinedApp);
};

int main(int argc, char* argv[]) {
  CefMainArgs main_args(argc, argv);

  // Single-exe model: check if we are a subprocess first
  CefRefPtr<LaufeyCombinedApp> app(new LaufeyCombinedApp());
  int exit_code = CefExecuteProcess(main_args, app, nullptr);
  if (exit_code >= 0) {
    return exit_code;
  }

  // Parse --runtime argument
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
      g_runtime_path = argv[++i];
    } else if (strncmp(argv[i], "--runtime=", 10) == 0) {
      g_runtime_path = argv[i] + 10;
    }
  }

  if (g_runtime_path.empty()) {
    const char* envPath = getenv("LAUFEY_RUNTIME_PATH");
    if (envPath) {
      g_runtime_path = envPath;
    }
  }

  // Check for headless / forked worker mode (skip CEF entirely)
  if (is_forked_worker() || is_cli_worker_command(argc, argv)) {
    return run_headless(g_runtime_path);
  }

  CefSettings settings;
  settings.no_sandbox = true;

  // Set cache path
  std::string cache_path = "/tmp/laufey_cef_" + std::to_string(getpid());
  CefString(&settings.root_cache_path) = cache_path;

  if (const char* port_env = getenv("LAUFEY_REMOTE_DEBUGGING_PORT")) {
    int port = atoi(port_env);
    if (port > 0 && port < 65536) {
      settings.remote_debugging_port = port;
    }
  }

  if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
    return 1;
  }

  CefRunMessageLoop();

  RuntimeLoader::GetInstance()->Shutdown();

  CefShutdown();

  return 0;
}
