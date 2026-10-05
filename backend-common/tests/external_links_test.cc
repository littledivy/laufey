// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// IsAllowedExternalLinkUrl (laufey_external_links.h): the native check on a
// URL a page asks the reserved __laufeyOpenExternal bridge call to open.
// Only absolute http(s) URLs with a host pass; everything the OS's open-URL
// primitive could turn into a program or a local file is refused. Plain
// string fixtures; nothing is opened. Plain asserts, no framework.

#include "laufey_external_links.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

int main() {
  // Allowed: absolute http(s) with a host, any case for the scheme.
  for (const char* ok : {
           "http://example.com",
           "https://example.com/",
           "https://example.com/a/b?q=1&r=%20#frag",
           "HTTPS://EXAMPLE.COM/",
           "Http://127.0.0.1:8080/x",
           "https://[::1]:443/",
           "https://user@example.com/",
           "https://xn--bcher-kva.example/",
       }) {
    EXPECT(IsAllowedExternalLinkUrl(ok));
  }

  // Refused: every other scheme, and anything without an http(s) host.
  for (const char* bad : {
           "",
           "file:///C:/Windows/System32/notepad.exe",
           "file://server/share/x.exe",
           "FILE:///etc/passwd",
           "\\\\host\\share\\x.exe",
           "//host/share/x",
           "javascript:alert(1)",
           "data:text/html,hi",
           "about:blank",
           "mailto:a@example.com",
           "ms-settings:",
           "search-ms:query=x",
           "myapp://open",
           "laufey://app/",
           "/relative/path",
           "relative/path",
           "example.com",
           "http:example.com",
           "http:/example.com",
           "http://",
           "https:///path",
           "https://\\\\host\\share",
           "https://?q",
           "https://#f",
           "https://@host",
           "https://:443",
           "httpx://example.com",
           "http//example.com",
       }) {
    EXPECT(!IsAllowedExternalLinkUrl(bad));
  }

  // Refused: whitespace and control characters anywhere.
  for (const char* bad : {
           " https://example.com",
           "https://example.com ",
           "https://exa mple.com",
           "https://example.com/\ta",
           "https://example.com/\na",
           "https://example.com/\ra",
           "https://example.com/\x7f",
           "https://example.com/\x01",
       }) {
    EXPECT(!IsAllowedExternalLinkUrl(bad));
  }
  std::string with_nul("https://example.com/");
  with_nul.push_back('\0');
  with_nul += "x";
  EXPECT(!IsAllowedExternalLinkUrl(with_nul));
  EXPECT(!IsAllowedExternalLinkUrl("https://example.com/" +
                                   std::string(40000, 'a')));

  std::printf("external_links_test: OK\n");
  return 0;
}
