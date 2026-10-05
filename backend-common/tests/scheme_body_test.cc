// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Tests for laufey_scheme_body.h, the bound on the custom-scheme request body
// that CEF (LaufeySchemeHandler::Open) and WebView2
// (HandleAppResourceRequested) buffer before the runtime's handler runs. The
// backends grow the body chunk by chunk with GrowSchemeRequestBody; past the
// cap it must refuse (and drop what it held) rather than keep growing.
//
// Plain asserts, no framework. Exits non-zero on the first failure. Build
// with `-std=c++17 -Ibackend-common/include`, as the `scheme-body` CI job
// does.

#include "laufey_scheme_body.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using laufey_common::GrowSchemeRequestBody;
using laufey_common::kMaxSchemeRequestBodyBytes;
using laufey_common::SchemeRequestBodyFits;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

constexpr size_t kSmallCap = 4096;

// Appends `n` bytes of a pattern starting at `seed`, the way the backends
// fill the room GrowSchemeRequestBody made. Returns its result.
bool Append(std::vector<uint8_t>* body, size_t n, uint8_t seed,
            size_t cap = kSmallCap) {
  size_t offset = body->size();
  if (!GrowSchemeRequestBody(body, n, cap))
    return false;
  for (size_t i = 0; i < n; ++i)
    (*body)[offset + i] = static_cast<uint8_t>(seed + i);
  return true;
}

// Chunks that add up to exactly the cap are all kept, byte for byte.
void TestUpToCapKept() {
  std::vector<uint8_t> body;
  size_t total = 0;
  uint8_t seed = 0;
  while (total < kSmallCap) {
    size_t n = kSmallCap - total < 1000 ? kSmallCap - total : 1000;
    EXPECT(Append(&body, n, seed));
    total += n;
    seed = static_cast<uint8_t>(seed + 7);
  }
  EXPECT(body.size() == kSmallCap);
  // Spot-check the first and last chunk's bytes.
  EXPECT(body[0] == 0 && body[999] == static_cast<uint8_t>(999));
  EXPECT(body[4000] == static_cast<uint8_t>(28));
}

// One byte past the cap is refused, and the body held so far is dropped.
void TestOneByteOverRefused() {
  std::vector<uint8_t> body;
  EXPECT(Append(&body, kSmallCap - 10, 1));
  EXPECT(Append(&body, 10, 2));
  EXPECT(!Append(&body, 1, 3));
  EXPECT(body.empty());
  EXPECT(body.capacity() == 0);
}

// A single chunk larger than the cap is refused before anything is held.
void TestSingleChunkOverRefused() {
  std::vector<uint8_t> body;
  EXPECT(!Append(&body, kSmallCap + 1, 0));
  EXPECT(body.empty());
}

// An empty chunk changes nothing, at the cap too.
void TestEmptyChunk() {
  std::vector<uint8_t> body;
  EXPECT(GrowSchemeRequestBody(&body, 0, kSmallCap));
  EXPECT(body.empty());
  EXPECT(Append(&body, kSmallCap, 5));
  EXPECT(GrowSchemeRequestBody(&body, 0, kSmallCap));
  EXPECT(body.size() == kSmallCap);
}

// The arithmetic cannot wrap around, whatever the sizes.
void TestFitsNoOverflow() {
  const size_t max = std::numeric_limits<size_t>::max();
  EXPECT(SchemeRequestBodyFits(0, kSmallCap, kSmallCap));
  EXPECT(!SchemeRequestBodyFits(0, kSmallCap + 1, kSmallCap));
  EXPECT(SchemeRequestBodyFits(kSmallCap, 0, kSmallCap));
  EXPECT(!SchemeRequestBodyFits(kSmallCap, 1, kSmallCap));
  EXPECT(!SchemeRequestBodyFits(kSmallCap + 1, 0, kSmallCap));
  EXPECT(!SchemeRequestBodyFits(1, max, kSmallCap));
  EXPECT(!SchemeRequestBodyFits(max, 1, kSmallCap));
  EXPECT(!SchemeRequestBodyFits(1, max));
  EXPECT(!SchemeRequestBodyFits(max, max));
}

// The default cap the backends use: 512 MiB, refused one byte past it
// without allocating it.
void TestDefaultCap() {
  EXPECT(kMaxSchemeRequestBodyBytes == size_t{512} * 1024 * 1024);
  EXPECT(SchemeRequestBodyFits(0, kMaxSchemeRequestBodyBytes));
  EXPECT(!SchemeRequestBodyFits(0, kMaxSchemeRequestBodyBytes + 1));
  EXPECT(!SchemeRequestBodyFits(kMaxSchemeRequestBodyBytes, 1));
  std::vector<uint8_t> body(16, 0xab);
  EXPECT(!GrowSchemeRequestBody(&body, kMaxSchemeRequestBodyBytes));
  EXPECT(body.empty());
}

}  // namespace

int main() {
  TestUpToCapKept();
  TestOneByteOverRefused();
  TestSingleChunkOverRefused();
  TestEmptyChunk();
  TestFitsNoOverflow();
  TestDefaultCap();
  std::printf("scheme_body_test: all tests passed\n");
  return 0;
}
