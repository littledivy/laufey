// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Buffering a custom-scheme (app://) request body. CEF and WebView2 read the
// whole body of a request before the runtime's scheme handler runs, so its
// scheme_request_read_body pulls never block. Without a bound, a page could
// make the host hold any amount of memory that way; past
// kMaxSchemeRequestBodyBytes the backend fails the request instead (the
// page's fetch rejects with a network error) and the handler never sees it.

#ifndef LAUFEY_SCHEME_BODY_H_
#define LAUFEY_SCHEME_BODY_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace laufey_common {

// The most request body bytes buffered for one custom-scheme request.
inline constexpr size_t kMaxSchemeRequestBodyBytes = 512u * 1024 * 1024;

// Whether a body already holding `held` bytes may take `more` without passing
// `cap` (and without overflowing).
inline bool SchemeRequestBodyFits(size_t held, size_t more,
                                  size_t cap = kMaxSchemeRequestBodyBytes) {
  return held <= cap && more <= cap - held;
}

// Grows `body` by `more` bytes, for the caller to fill from its old size on,
// and returns true. When that would take it past `cap`, empties `body`
// (releasing its memory) and returns false: the caller fails the request.
inline bool GrowSchemeRequestBody(std::vector<uint8_t>* body, size_t more,
                                  size_t cap = kMaxSchemeRequestBodyBytes) {
  if (!SchemeRequestBodyFits(body->size(), more, cap)) {
    std::vector<uint8_t>().swap(*body);
    return false;
  }
  body->resize(body->size() + more);
  return true;
}

}  // namespace laufey_common

#endif  // LAUFEY_SCHEME_BODY_H_
