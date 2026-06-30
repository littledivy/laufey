// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
// Shared policy for redirecting external link navigations to the OS browser.
//
// Clicking an `<a href="https://…">` inside a webview would otherwise replace
// the app's view with the remote page. To match the behavior apps expect
// (external links open in the user's browser, the app keeps running), every
// page is injected with a standards-based Navigation API listener that cancels
// such navigations and hands the URL to the native side via a reserved bridge
// call.
//
// This header is the single source of truth shared by every backend: the
// system WebView backends (webview/src/init_script.h) and the CEF renderer
// (cef/src/render_process_handler.cc). The reserved method name is also matched
// natively by each backend's call dispatcher, which routes it to the
// platform's open-in-browser primitive instead of the runtime.

#ifndef LAUFEY_EXTERNAL_LINKS_H_
#define LAUFEY_EXTERNAL_LINKS_H_

#include <string>

// Reserved bridge method. The injected listener calls
// `window[ns].__laufeyOpenExternal(url)`; each backend intercepts a call with
// this path before it reaches the runtime and opens `url` in the OS browser.
#define LAUFEY_OPEN_EXTERNAL_METHOD "__laufeyOpenExternal"

// Builds the page-side interceptor for the namespace `ns` (the global the
// laufey bridge proxy is installed under, e.g. "laufey").
//
// Policy (hardcoded by design — see the conversation around this feature):
// only user-initiated, cancelable, cross-origin http(s) navigations are
// redirected. Same-origin navigations (SPA routing, multi-page apps) and the
// app's own `laufey://` scheme stay in the view. Downloads, reloads, history
// traversals and fragment changes are left alone.
//
// Two mechanisms, picked at runtime by capability:
//   * Navigation API (`window.navigation`): the modern, comprehensive path —
//     also catches `location.href = …`, form submits, etc. Used on Chromium
//     (CEF) and WebView2.
//   * Click-capture fallback: used when the Navigation API is absent (notably
//     WKWebView on macOS/iOS, which does not expose it). Intercepts plain left
//     clicks on `<a>` elements. Narrower than the Navigation API but covers the
//     "clicked a link" case across every engine.
// Exactly one runs, so a link is never handled twice.
//
// Both only cover same-window navigations — `target="_blank"` and
// `window.open()` never reach either and are handled by each backend's
// new-window hook instead.
inline std::string BuildExternalLinkInterceptScript(const std::string& ns) {
  return R"JS(
(function() {
  var ns = ")JS" +
         ns + R"JS(";

  // True for http(s) destinations whose origin differs from the document's.
  // location.origin is "null" for opaque-origin documents (e.g. data: URLs),
  // so any real http(s) link counts as external there.
  function isExternal(href) {
    var dest;
    try { dest = new URL(href, location.href); } catch (e) { return null; }
    if (dest.protocol !== 'http:' && dest.protocol !== 'https:') return null;
    if (dest.origin === location.origin) return null;
    return dest.href;
  }

  function openExternal(href) {
    var bridge = window[ns];
    if (bridge) {
      bridge.__laufeyOpenExternal(href);
    }
  }

  if (window.navigation &&
      typeof window.navigation.addEventListener === 'function') {
    window.navigation.addEventListener('navigate', function(e) {
      try {
        if (!e.userInitiated || !e.cancelable || e.downloadRequest ||
            e.hashChange || e.navigationType === 'reload' ||
            e.navigationType === 'traverse') {
          return;
        }
        var href = isExternal(e.destination.url);
        if (href) {
          e.preventDefault();
          openExternal(href);
        }
      } catch (err) {
        // Never let the policy break in-page navigation.
      }
    });
    return;
  }

  // Fallback: no Navigation API. Intercept plain left clicks on anchors.
  document.addEventListener('click', function(e) {
    try {
      if (e.defaultPrevented || e.button !== 0) return;
      var el = e.target;
      while (el && el.nodeType === 1 &&
             (el.tagName === undefined || el.tagName.toUpperCase() !== 'A')) {
        el = el.parentNode;
      }
      if (!el || !el.tagName || el.tagName.toUpperCase() !== 'A') return;
      // `target="_blank"` is handled by the backend's new-window hook.
      var target = el.getAttribute('target');
      if (target && target !== '_self') return;
      var href = el.href;
      if (!href) return;
      var external = isExternal(href);
      if (external) {
        e.preventDefault();
        openExternal(external);
      }
    } catch (err) {
      // Never let the policy break in-page navigation.
    }
  }, true);
})();
)JS";
}

#endif  // LAUFEY_EXTERNAL_LINKS_H_
