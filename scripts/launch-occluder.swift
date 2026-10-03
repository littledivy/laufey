// Covers every screen with an ordinary window from another process, the way
// an editor or a terminal is in front when an app is started from it. Used by
// scripts/native-e2e-run.sh --launch-visibility (examples/native_e2e
// launch_checks.rs): the app launched next has to bring its first window to
// the front, or WebKit reads that window as occluded and its page as hidden.
//
// Prints "occluder ready ..." once its windows are up, then runs until killed.
//
//   swiftc -O scripts/launch-occluder.swift -o launch-occluder

import AppKit

final class Occluder: NSObject, NSApplicationDelegate {
  var windows: [NSWindow] = []

  func applicationDidFinishLaunching(_ notification: Notification) {
    for screen in NSScreen.screens {
      let window = NSWindow(
        contentRect: screen.frame, styleMask: [.borderless],
        backing: .buffered, defer: false)
      window.isReleasedWhenClosed = false
      window.backgroundColor = .darkGray
      window.setFrame(screen.frame, display: true)
      windows.append(window)
    }
    if #available(macOS 14.0, *) {
      NSApp.activate()
    } else {
      NSApp.activate(ignoringOtherApps: true)
    }
    for window in windows {
      window.makeKeyAndOrderFront(nil)
      window.orderFrontRegardless()
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
      let visible = self.windows.allSatisfy {
        $0.occlusionState.contains(.visible)
      }
      print(
        "occluder ready: \(self.windows.count) window(s), active=\(NSApp.isActive), visible=\(visible)"
      )
      fflush(stdout)
    }
  }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let delegate = Occluder()
app.delegate = delegate
app.run()
