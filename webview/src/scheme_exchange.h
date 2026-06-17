// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef LAUFEY_SCHEME_EXCHANGE_H_
#define LAUFEY_SCHEME_EXCHANGE_H_

#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The custom standard scheme laufey serves an app over (e.g. "app://...").
#define LAUFEY_APP_SCHEME "app"

// Abstract, backend-agnostic handle for one in-flight scheme exchange. Each
// webview backend subclasses this around its native request/response objects
// (WKURLSchemeTask, WebKitURISchemeRequest, WebView2 deferral, Android
// InputStream). The RuntimeLoader's scheme_* vtable functions cast the opaque
// laufey_scheme_exchange_t* to this base and dispatch through the virtuals.
class SchemeExchangeBase {
 public:
  virtual ~SchemeExchangeBase() = default;

  // Pull up to `cap` bytes of the request body. Returns bytes read (>0), 0 at
  // EOF, or <0 on error.
  virtual intptr_t ReadRequestBody(uint8_t* buf, size_t cap) = 0;

  // Send the response status + headers (flat name\0value\0... encoding). Once.
  virtual void Begin(int status, const char* headers, size_t headers_len) = 0;

  // Append response body bytes. Returns bytes accepted, or <0 if the webview
  // went away.
  virtual intptr_t WriteResponse(const uint8_t* buf, size_t len) = 0;

  // Complete the response and release this exchange (typically `delete this`).
  virtual void Finish() = 0;
};

// Decode the flat name\0value\0...\0 header encoding into pairs.
inline std::vector<std::pair<std::string, std::string>> LaufeyParseFlatHeaders(
    const char* headers, size_t len) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t i = 0;
  while (i < len) {
    size_t name_end = i;
    while (name_end < len && headers[name_end] != '\0')
      name_end++;
    if (name_end >= len)
      break;
    size_t value_start = name_end + 1;
    size_t value_end = value_start;
    while (value_end < len && headers[value_end] != '\0')
      value_end++;
    if (value_end > len)
      break;
    out.emplace_back(
        std::string(headers + i, name_end - i),
        std::string(headers + value_start, value_end - value_start));
    i = value_end + 1;
  }
  return out;
}

// Encode header pairs into the flat name\0value\0...\0 wire format, lowercasing
// names. Used to hand a native request's headers to the runtime.
inline std::string LaufeyFlattenHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers) {
  std::string out;
  for (const auto& [name, value] : headers) {
    std::string lname = name;
    for (auto& c : lname)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out.append(lname);
    out.push_back('\0');
    out.append(value);
    out.push_back('\0');
  }
  return out;
}

#endif  // LAUFEY_SCHEME_EXCHANGE_H_
