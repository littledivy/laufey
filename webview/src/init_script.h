// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

// The JavaScript bridge injected into every page is identical across all
// platform webview backends; only the host-specific postMessage expression and
// the namespace differ. Keep one copy here instead of duplicating it in
// webview_macos.mm / webview_linux.cc / webview_windows.cc.

#ifndef LAUFEY_WEBVIEW_INIT_SCRIPT_H_
#define LAUFEY_WEBVIEW_INIT_SCRIPT_H_

#include <cstdint>
#include <string>

// The host->page side of the bridge protocol. These build the JS statements
// that call the functions defined by BuildInitScript() below, so the function
// names live in exactly one place. Each platform backend builds the statement
// and hands it to its own evaluate-JavaScript call.

// window.__laufeyRespond(callId, result, error): resolve/reject a pending call.
inline std::string BuildRespondScript(uint64_t call_id,
                                      const std::string& result_json,
                                      const std::string& error_json,
                                      bool is_error) {
  if (is_error) {
    return "window.__laufeyRespond(" + std::to_string(call_id) + ", null, " +
           error_json + ");";
  }
  return "window.__laufeyRespond(" + std::to_string(call_id) + ", " + result_json +
         ", null);";
}

// window.__laufeyInvokeCallback(callbackId, args): call a stored JS callback.
inline std::string BuildInvokeCallbackScript(uint64_t callback_id,
                                             const std::string& args_json) {
  return "window.__laufeyInvokeCallback(" + std::to_string(callback_id) + ", " +
         args_json + ");";
}

// window.__laufeyReleaseCallback(callbackId): drop a stored JS callback.
inline std::string BuildReleaseCallbackScript(uint64_t callback_id) {
  return "window.__laufeyReleaseCallback(" + std::to_string(callback_id) + ");";
}

// Builds the page init script. `ns` is the global namespace the proxy is
// installed under (e.g. "laufey"); `postMessage` is the platform-specific
// statement that ships the {callId, path, args} message to the host.
inline std::string BuildInitScript(const std::string& ns,
                                   const std::string& postMessage) {
  return R"JS(
(function() {
  const pendingCalls = new Map();
  let nextCallId = 1;

  function createLaufeyProxy(path = []) {
    return new Proxy(function() {}, {
      get(target, prop) {
        if (prop === 'then' || prop === 'catch' || prop === 'finally' ||
            prop === 'constructor' || prop === Symbol.toStringTag) {
          return undefined;
        }
        return createLaufeyProxy([...path, prop]);
      },
      apply(target, thisArg, args) {
        return new Promise((resolve, reject) => {
          const callId = nextCallId++;
          pendingCalls.set(callId, { resolve, reject });

          const processedArgs = args.map(arg => {
            if (typeof arg === 'function') {
              const cbId = nextCallId++;
              window.__laufeyCallbacks = window.__laufeyCallbacks || {};
              window.__laufeyCallbacks[cbId] = arg;
              return { __callback__: String(cbId) };
            }
            if (arg instanceof ArrayBuffer) {
              const bytes = new Uint8Array(arg);
              let binary = '';
              bytes.forEach(b => binary += String.fromCharCode(b));
              return { __binary__: btoa(binary) };
            }
            if (arg instanceof Uint8Array) {
              let binary = '';
              arg.forEach(b => binary += String.fromCharCode(b));
              return { __binary__: btoa(binary) };
            }
            return arg;
          });

          )JS" +
         postMessage + R"JS(
        });
      }
    });
  }

  window[")JS" +
         ns + R"JS("] = createLaufeyProxy();

  window.__laufeyRespond = function(callId, result, error) {
    const pending = pendingCalls.get(callId);
    if (pending) {
      pendingCalls.delete(callId);
      if (error) {
        pending.reject(new Error(error));
      } else {
        function convertBinary(obj) {
          if (obj && typeof obj === 'object') {
            if (obj.__binary__) {
              const binary = atob(obj.__binary__);
              const bytes = new Uint8Array(binary.length);
              for (let i = 0; i < binary.length; i++) {
                bytes[i] = binary.charCodeAt(i);
              }
              return bytes.buffer;
            }
            if (Array.isArray(obj)) {
              return obj.map(convertBinary);
            }
            const result = {};
            for (const key in obj) {
              result[key] = convertBinary(obj[key]);
            }
            return result;
          }
          return obj;
        }
        pending.resolve(convertBinary(result));
      }
    }
  };

  window.__laufeyInvokeCallback = function(callbackId, args) {
    const cb = window.__laufeyCallbacks && window.__laufeyCallbacks[callbackId];
    if (cb) {
      cb.apply(null, args);
    }
  };

  window.__laufeyReleaseCallback = function(callbackId) {
    if (window.__laufeyCallbacks) {
      delete window.__laufeyCallbacks[callbackId];
    }
  };

})();
)JS";
}

#endif  // LAUFEY_WEBVIEW_INIT_SCRIPT_H_
