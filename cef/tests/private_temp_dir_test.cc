// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// LaufeyMakePrivateTempDir (cef/src/private_temp_dir.h), the CEF Linux
// profile directory when the app gives no data directory: a fresh 0700
// directory with an unpredictable name, never a fixed or reused path. Built
// and run on its own (see the private-temp-dir job in ci.yml):
//
//   c++ -std=c++17 -Icef/src cef/tests/private_temp_dir_test.cc -o t && ./t

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "private_temp_dir.h"

static int g_failures = 0;

#define EXPECT(cond)                                                          \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                           \
    }                                                                         \
  } while (0)

static bool StartsWith(const std::string& s, const std::string& prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

int main() {
  char tmpl[] = "/tmp/laufey_private_dir_test_XXXXXX";
  char* root = mkdtemp(tmpl);
  EXPECT(root != nullptr);
  if (!root) {
    return 1;
  }
  const std::string prefix = std::string(root) + "/laufey_cef_";

  // Two calls: two different directories, each `<prefix>` + a 6-character
  // random suffix (a trailing slash on the parent is fine).
  std::string a = LaufeyMakePrivateTempDir(root, "laufey_cef_");
  std::string b =
      LaufeyMakePrivateTempDir(std::string(root) + "/", "laufey_cef_");
  EXPECT(!a.empty() && !b.empty() && a != b);
  EXPECT(StartsWith(a, prefix));
  EXPECT(a.size() == prefix.size() + 6);
  EXPECT(StartsWith(b, prefix));
  EXPECT(b.size() == prefix.size() + 6);
  // Not the predictable per-pid name.
  EXPECT(a != prefix + std::to_string(getpid()));

  // A new owner-only directory.
  struct stat st;
  EXPECT(stat(a.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
  EXPECT((st.st_mode & 0777) == 0700);
  EXPECT(st.st_uid == getuid());

  // A parent that doesn't exist gives "", not a path.
  EXPECT(
      LaufeyMakePrivateTempDir(std::string(root) + "/missing", "x_").empty());

  // "" means $TMPDIR when it is absolute, else /tmp.
  const char* old_tmpdir = getenv("TMPDIR");
  std::string saved = old_tmpdir ? old_tmpdir : "";
  setenv("TMPDIR", root, 1);
  std::string c = LaufeyMakePrivateTempDir("", "laufey_cef_");
  EXPECT(StartsWith(c, prefix));
  setenv("TMPDIR", "relative", 1);
  std::string d = LaufeyMakePrivateTempDir("", "laufey_private_dir_test_");
  EXPECT(StartsWith(d, "/tmp/laufey_private_dir_test_"));
  if (old_tmpdir) {
    setenv("TMPDIR", saved.c_str(), 1);
  } else {
    unsetenv("TMPDIR");
  }

  for (const std::string& dir : {a, b, c, d}) {
    if (!dir.empty()) {
      rmdir(dir.c_str());
    }
  }
  rmdir(root);

  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("private_temp_dir_test: ok\n");
  return 0;
}
