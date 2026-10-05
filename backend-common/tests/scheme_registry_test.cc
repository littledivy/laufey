// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for laufey_scheme_registry.h. Plain asserts, no framework: run
// via `ctest --test-dir webview/build` (or cef/build). Exits non-zero on the
// first failure.

#include "laufey_scheme_registry.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using laufey_common::IsReservedSchemeName;
using laufey_common::IsSchemeNameGrammar;
using laufey_common::IsValidSchemeName;
using laufey_common::JoinForwardedSchemes;
using laufey_common::MergeForwardedSchemes;
using laufey_common::MergeSchemeLists;
using laufey_common::NormalizeSchemeName;
using laufey_common::ParseSchemeList;
using laufey_common::SchemeRegistry;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static void TestSchemeNameGrammar() {
  EXPECT(IsValidSchemeName("app"));
  EXPECT(IsValidSchemeName("mail2"));
  EXPECT(IsValidSchemeName("my-app.v2+x"));
  EXPECT(IsValidSchemeName("A"));
  EXPECT(IsValidSchemeName("MyApp"));

  EXPECT(!IsValidSchemeName(""));
  EXPECT(!IsValidSchemeName("1app"));  // must start with a letter
  EXPECT(!IsValidSchemeName("-app"));
  EXPECT(!IsValidSchemeName("app://"));  // name only, no separator
  EXPECT(!IsValidSchemeName("my app"));
  EXPECT(!IsValidSchemeName("my_app"));  // "_" is not in the RFC grammar
  EXPECT(!IsValidSchemeName("app/x"));
  EXPECT(!IsValidSchemeName(std::string("app\0x", 5)));

  // Schemes the engines already give a meaning to can't be taken over, in
  // any case; they still follow the grammar.
  for (const char* reserved : {"http",
                               "https",
                               "file",
                               "ws",
                               "wss",
                               "data",
                               "blob",
                               "javascript",
                               "about",
                               "ftp",
                               "filesystem",
                               "chrome",
                               "chrome-extension",
                               "chrome-untrusted",
                               "devtools",
                               "view-source",
                               "HTTPS",
                               "JavaScript",
                               "File",
                               "Data",
                               "BLOB",
                               "View-Source"}) {
    EXPECT(IsSchemeNameGrammar(reserved));
    EXPECT(IsReservedSchemeName(reserved));
    EXPECT(!IsValidSchemeName(reserved));
  }
  // Near misses and OS-only schemes stay allowed.
  for (const char* allowed :
       {"https2", "httpx", "xhttp", "files", "app", "mailto", "tel",
        "javascript-app", "about-app", "wss.app"}) {
    EXPECT(!IsReservedSchemeName(allowed));
    EXPECT(IsValidSchemeName(allowed));
  }

  EXPECT(NormalizeSchemeName("MyApp") == "myapp");
  EXPECT(NormalizeSchemeName("app") == "app");
  EXPECT(NormalizeSchemeName("Mail2-Web.X+Y") == "mail2-web.x+y");
}

static void TestRegistryDefaults() {
  SchemeRegistry registry;
  EXPECT(registry.size() == 1);
  EXPECT(registry.Contains("app"));
  EXPECT(registry.Contains("APP"));
  EXPECT(!registry.Contains("mail2"));
  std::vector<std::string> snapshot = registry.Snapshot();
  EXPECT(snapshot.size() == 1);
  EXPECT(snapshot[0] == "app");
}

static void TestRegistryAdd() {
  SchemeRegistry registry;

  // "app" is pre-registered: re-adding it is a no-op.
  EXPECT(!registry.Add("app"));
  EXPECT(!registry.Add("APP"));
  EXPECT(registry.size() == 1);

  // A new scheme reports true once, then false (already present), and is
  // stored lowercase regardless of how it was spelled.
  EXPECT(registry.Add("mail2"));
  EXPECT(!registry.Add("mail2"));
  EXPECT(!registry.Add("Mail2"));
  EXPECT(registry.Contains("mail2"));
  EXPECT(registry.Contains("MAIL2"));

  // Invalid names are rejected without being stored.
  EXPECT(!registry.Add(""));
  EXPECT(!registry.Add("bad scheme"));
  EXPECT(!registry.Add("app://"));
  EXPECT(!registry.Contains("bad scheme"));

  // Snapshot keeps "app" first and then registration order.
  EXPECT(registry.Add("Zeta"));
  EXPECT(registry.Add("alpha"));
  std::vector<std::string> snapshot = registry.Snapshot();
  EXPECT(snapshot.size() == 4);
  EXPECT(snapshot[0] == "app");
  EXPECT(snapshot[1] == "mail2");
  EXPECT(snapshot[2] == "zeta");
  EXPECT(snapshot[3] == "alpha");
  EXPECT(registry.size() == 4);
}

static void TestRegistryConcurrentAdd() {
  // The runtime registers from its own thread while the UI thread snapshots.
  // Hammer Add/Snapshot from several threads and check the final set is
  // exactly the union, with each name present once.
  SchemeRegistry registry;
  static constexpr int kThreads = 8;
  static constexpr int kPerThread = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&registry, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        // Overlapping names across threads: only one Add per name wins.
        registry.Add("s" + std::to_string(i % 50));
        registry.Add("thread" + std::to_string(t));
        std::vector<std::string> snapshot = registry.Snapshot();
        EXPECT(!snapshot.empty() && snapshot[0] == "app");
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT(registry.size() == 1 + 50 + kThreads);
  for (int i = 0; i < 50; ++i) {
    EXPECT(registry.Contains("s" + std::to_string(i)));
  }
  for (int t = 0; t < kThreads; ++t) {
    EXPECT(registry.Contains("thread" + std::to_string(t)));
  }
}

static void TestGlobalInstance() {
  SchemeRegistry* a = SchemeRegistry::GetInstance();
  SchemeRegistry* b = SchemeRegistry::GetInstance();
  EXPECT(a == b);
  EXPECT(a->Contains("app"));
}

static void TestParseSchemeList() {
  std::vector<std::string> rejected;

  std::vector<std::string> empty = ParseSchemeList("", &rejected);
  EXPECT(empty.empty());
  EXPECT(rejected.empty());

  std::vector<std::string> one = ParseSchemeList("mail2", &rejected);
  EXPECT(one.size() == 1 && one[0] == "mail2");

  // Whitespace, case, duplicates and empty entries.
  std::vector<std::string> many =
      ParseSchemeList(" Mail2 , other,,mail2, third ", &rejected);
  EXPECT(many.size() == 3);
  EXPECT(many[0] == "mail2");
  EXPECT(many[1] == "other");
  EXPECT(many[2] == "third");
  EXPECT(rejected.empty());

  // Invalid entries are reported and skipped; valid neighbours survive.
  std::vector<std::string> mixed =
      ParseSchemeList("good,1bad,also good,app://,fine", &rejected);
  EXPECT(mixed.size() == 2);
  EXPECT(mixed[0] == "good");
  EXPECT(mixed[1] == "fine");
  EXPECT(rejected.size() == 3);
  EXPECT(rejected[0] == "1bad");
  EXPECT(rejected[1] == "also good");
  EXPECT(rejected[2] == "app://");

  // Reserved schemes (the switch, LAUFEY_CUSTOM_SCHEMES) are rejected too.
  rejected.clear();
  std::vector<std::string> reserved =
      ParseSchemeList("https,myapp,JavaScript", &rejected);
  EXPECT(reserved.size() == 1 && reserved[0] == "myapp");
  EXPECT(rejected.size() == 2);

  // A null `rejected` is allowed.
  std::vector<std::string> quiet = ParseSchemeList("ok,not ok");
  EXPECT(quiet.size() == 1 && quiet[0] == "ok");
}

// The CEF backend's startup declaration (cef/src/custom_schemes.cc): the
// --laufey-custom-schemes switch value and LAUFEY_CUSTOM_SCHEMES are merged
// behind the built-in "app", and the result minus "app" is forwarded to child
// processes.
static void TestMergeSchemeLists() {
  std::vector<std::string> rejected;

  // Nothing declared: just "app".
  std::vector<std::string> none = MergeSchemeLists({}, &rejected);
  EXPECT(none.size() == 1 && none[0] == "app");
  EXPECT(MergeSchemeLists({"", " , "}, &rejected).size() == 1);
  EXPECT(rejected.empty());

  // Switch then env: "app" stays first, order is first-seen, case and
  // duplicates (within and across the two sources, "app" included) collapse.
  std::vector<std::string> merged =
      MergeSchemeLists({"MyApp,app,other", "other, THIRD ,myapp"}, &rejected);
  EXPECT(merged.size() == 4);
  EXPECT(merged[0] == "app");
  EXPECT(merged[1] == "myapp");
  EXPECT(merged[2] == "other");
  EXPECT(merged[3] == "third");
  EXPECT(rejected.empty());

  // Invalid names from either source are reported and dropped.
  std::vector<std::string> mixed =
      MergeSchemeLists({"good,bad name", "1bad,fine"}, &rejected);
  EXPECT(mixed.size() == 3);
  EXPECT(mixed[1] == "good" && mixed[2] == "fine");
  EXPECT(rejected.size() == 2);
  EXPECT(rejected[0] == "bad name" && rejected[1] == "1bad");

  // A null `rejected` is allowed.
  EXPECT(MergeSchemeLists({"ok,not ok"}).size() == 2);
}

static void TestJoinForwardedSchemes() {
  // "app" alone forwards nothing (children declare it themselves).
  EXPECT(JoinForwardedSchemes({"app"}).empty());
  EXPECT(JoinForwardedSchemes({}).empty());
  EXPECT(JoinForwardedSchemes({"app", "myapp"}) == "myapp");
  EXPECT(JoinForwardedSchemes({"app", "myapp", "other"}) == "myapp,other");
  // The forwarded value parses back to the same declaration in the child.
  std::vector<std::string> declared =
      MergeSchemeLists({"myapp,other", "third"});
  EXPECT(MergeSchemeLists({JoinForwardedSchemes(declared)}) == declared);
}

static void TestMergeForwardedSchemes() {
  // No switch on the child yet: the declared list, as JoinForwardedSchemes.
  EXPECT(MergeForwardedSchemes("", {"app", "myapp"}) == "myapp");
  EXPECT(MergeForwardedSchemes("", {"app"}).empty());
  // The child already has some: keep them, add the rest, no duplicates.
  EXPECT(MergeForwardedSchemes("myapp", {"app", "myapp", "other"}) ==
         "myapp,other");
  EXPECT(MergeForwardedSchemes("other,myapp", {"app", "myapp", "third"}) ==
         "other,myapp,third");
  // Case-insensitive, and "app" / invalid entries on the child are dropped.
  EXPECT(MergeForwardedSchemes("MyApp,app,not ok", {"app", "myapp"}) ==
         "myapp");
  // Nothing to add leaves the child's value as it was.
  EXPECT(MergeForwardedSchemes("myapp,other", {"app", "other"}) ==
         "myapp,other");
}

int main() {
  TestSchemeNameGrammar();
  TestRegistryDefaults();
  TestRegistryAdd();
  TestRegistryConcurrentAdd();
  TestGlobalInstance();
  TestParseSchemeList();
  TestMergeSchemeLists();
  TestJoinForwardedSchemes();
  TestMergeForwardedSchemes();
  std::printf("scheme_registry_test: all tests passed\n");
  return 0;
}
