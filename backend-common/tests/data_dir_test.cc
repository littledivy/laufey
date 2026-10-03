// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the app data directory resolver (src/data_dir.cc). No test
// framework: compile this file with src/data_dir.cc (plus src/strings_win.cc
// and shell32.lib ole32.lib on Windows), with backend-common/include and
// capi/include on the include path, and run it. CI does this in the `test`
// job. Exits non-zero if any expectation fails.

#include "laufey_backend_common.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace laufey_common;

static int g_failures = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

#ifdef _WIN32
static const std::string kBase = "C:\\Users\\u\\AppData\\Local";
static const std::string kSep = "\\";
static const std::string kAbsDir = "D:\\data\\my app";
#else
static const std::string kBase = "/home/u/.local/share";
static const std::string kSep = "/";
static const std::string kAbsDir = "/srv/data/my app";
#endif

static std::string Resolve(const std::string& data_dir,
                           const std::string& app_id, const std::string& base,
                           size_t* warning_count) {
  std::vector<std::string> warnings;
  std::string r = ResolveAppDataDirFrom(data_dir, app_id, base, &warnings);
  *warning_count = warnings.size();
  return r;
}

static void TestSafeAppId() {
  EXPECT(IsSafeAppId("com.example.app"));
  EXPECT(IsSafeAppId("my-app_2"));
  EXPECT(IsSafeAppId("..."));  // a plain name, cannot climb
  EXPECT(!IsSafeAppId(""));
  EXPECT(!IsSafeAppId("."));
  EXPECT(!IsSafeAppId(".."));
  EXPECT(!IsSafeAppId("a/b"));
  EXPECT(!IsSafeAppId("a\\b"));
  EXPECT(!IsSafeAppId("../etc"));
  EXPECT(!IsSafeAppId("my app"));
  EXPECT(!IsSafeAppId("C:"));
  EXPECT(!IsSafeAppId(std::string("a\0b", 3)));
  EXPECT(!IsSafeAppId("caf\xc3\xa9"));  // non-ASCII
}

static void TestAbsolutePath() {
  EXPECT(IsAbsolutePath(kAbsDir));
  EXPECT(!IsAbsolutePath(""));
  EXPECT(!IsAbsolutePath("relative/dir"));
  EXPECT(!IsAbsolutePath("./x"));
  EXPECT(!IsAbsolutePath(kAbsDir + std::string("\0x", 2)));
#ifdef _WIN32
  EXPECT(IsAbsolutePath("C:/x"));
  EXPECT(IsAbsolutePath("\\\\server\\share"));
  EXPECT(!IsAbsolutePath("C:x"));
  EXPECT(!IsAbsolutePath("\\x"));
#endif
}

static void TestResolve() {
  size_t warnings = 0;

  // Nothing configured: no directory, no warning (backward compatible).
  EXPECT(Resolve("", "", kBase, &warnings).empty());
  EXPECT(warnings == 0);

  // App id -> <base>/<id>.
  EXPECT(Resolve("", "com.example.app", kBase, &warnings) ==
         kBase + kSep + "com.example.app");
  EXPECT(warnings == 0);

  // LAUFEY_DATA_DIR wins over the app id, trailing separators stripped.
  EXPECT(Resolve(kAbsDir, "com.example.app", kBase, &warnings) == kAbsDir);
  EXPECT(Resolve(kAbsDir + kSep + kSep, "", kBase, &warnings) == kAbsDir);
  EXPECT(warnings == 0);

  // Relative LAUFEY_DATA_DIR is ignored with a warning, falling back to the
  // app id (or to nothing).
  EXPECT(Resolve("rel/dir", "com.example.app", kBase, &warnings) ==
         kBase + kSep + "com.example.app");
  EXPECT(warnings == 1);
  EXPECT(Resolve("rel/dir", "", kBase, &warnings).empty());
  EXPECT(warnings == 1);

  // Unsafe ids never produce a path (so nothing can escape the base).
  const char* unsafe[] = {"..",   ".",      "../../etc", "a/b",
                          "a\\b", "my app", "/abs"};
  for (const char* id : unsafe) {
    EXPECT(Resolve("", id, kBase, &warnings).empty());
    EXPECT(warnings == 1);
  }
  // Both inputs bad: one warning each.
  EXPECT(Resolve("rel", "..", kBase, &warnings).empty());
  EXPECT(warnings == 2);

  // Unknown platform base: warn, no directory.
  EXPECT(Resolve("", "com.example.app", "", &warnings).empty());
  EXPECT(warnings == 1);
}

static void TestJoinPath() {
  EXPECT(JoinPath(kBase, "CEF") == kBase + kSep + "CEF");
  EXPECT(JoinPath(kBase + kSep, "CEF") == kBase + kSep + "CEF");
}

#ifndef _WIN32
static void TestEnsureDirectory() {
  char tmpl[] = "/tmp/laufey_data_dir_test_XXXXXX";
  char* root = mkdtemp(tmpl);
  EXPECT(root != nullptr);
  if (!root)
    return;
  std::string leaf = JoinPath(JoinPath(root, "app.id"), "CEF");
  EXPECT(EnsureDirectory(leaf));
  EXPECT(EnsureDirectory(leaf));  // idempotent
  struct stat st;
  EXPECT(stat(leaf.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
  EXPECT((st.st_mode & 0777) == 0700);
  EXPECT(!EnsureDirectory("relative/dir"));
  // A regular file in the way is an error, not a directory.
  std::string file = JoinPath(root, "file");
  FILE* f = std::fopen(file.c_str(), "w");
  if (f)
    std::fclose(f);
  EXPECT(!EnsureDirectory(JoinPath(file, "sub")));
  std::remove(file.c_str());
  rmdir(leaf.c_str());
  rmdir(JoinPath(root, "app.id").c_str());
  rmdir(root);
}
#endif

int main() {
  TestSafeAppId();
  TestAbsolutePath();
  TestResolve();
  TestJoinPath();
#ifndef _WIN32
  TestEnsureDirectory();
#endif
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("data_dir_test: all passed\n");
  return 0;
}
