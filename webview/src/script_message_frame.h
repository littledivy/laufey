// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

// Which WKScriptMessages the bridge accepts. Shared by the macOS and iOS
// WKWebView backends (webview_macos.mm / webview_ios.mm); Objective-C++ only.

#ifndef LAUFEY_WEBVIEW_SCRIPT_MESSAGE_FRAME_H_
#define LAUFEY_WEBVIEW_SCRIPT_MESSAGE_FRAME_H_

#import <WebKit/WebKit.h>

// Whether a document with `origin` may be the web view's page at `url`: the
// same scheme, host and port (a missing port is the scheme's default, which
// WebKit reports as 0). A page without a host (HTML loaded with no base URL is
// about:blank, a data: URL has an opaque origin) has no origin to compare, so
// only the main-frame check applies to it.
static inline bool LaufeyOriginMatchesURL(WKSecurityOrigin* origin,
                                          NSURL* url) {
  if (!origin)
    return false;
  if (!url || url.host.length == 0)
    return true;
  NSInteger port = url.port ? url.port.integerValue : 0;
  return [origin.protocol caseInsensitiveCompare:url.scheme] == NSOrderedSame &&
         [origin.host caseInsensitiveCompare:url.host] == NSOrderedSame &&
         origin.port == port;
}

// WebKit exposes a script message handler to every frame (only the bridge
// script is injected forMainFrameOnly), so a sub-frame, cross-origin content
// included, could post to the handler directly. Accept a message only from
// the main frame, and only while its document is still the web view's page
// (not a message from a page navigated away from). Mirrors WebView2's source
// check and CEF's main-frame-only binding.
static inline bool LaufeyIsMainFrameMessage(WKScriptMessage* message) {
  WKFrameInfo* frame = message.frameInfo;
  return frame && frame.isMainFrame &&
         LaufeyOriginMatchesURL(frame.securityOrigin, message.webView.URL);
}

#endif  // LAUFEY_WEBVIEW_SCRIPT_MESSAGE_FRAME_H_
