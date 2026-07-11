#include <cstdio>
#include <cstring>

#include "lib/JsonParser/CatalogJsonParser.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                           \
  do {                                                                            \
    auto _a = (a);                                                                \
    auto _b = (b);                                                                \
    if (_a != _b) {                                                               \
      fprintf(stderr, "  FAIL: %s:%d: %s != expected\n", __FILE__, __LINE__, #a); \
      testsFailed++;                                                              \
      return;                                                                     \
    }                                                                             \
  } while (0)

#define ASSERT_STREQ(a, b)                                                              \
  do {                                                                                  \
    const char* _a = (a);                                                               \
    const char* _b = (b);                                                               \
    if (strcmp(_a, _b) != 0) {                                                          \
      fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); \
      testsFailed++;                                                                    \
      return;                                                                           \
    }                                                                                   \
  } while (0)

#define ASSERT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      testsFailed++;                                                     \
      return;                                                            \
    }                                                                    \
  } while (0)

#define PASS() testsPassed++

// ============================================================================
// Real catalog payload as served from https://crossink.uxj.io/catalog
// ============================================================================

static const char* kRealCatalog = R"({
  "schema_version": 1,
  "releases": [
    {
      "id": "stable-1.4.0-tiny",
      "channel": "stable",
      "name": "1.4.0",
      "version": "1.4.0",
      "variant": "tiny",
      "released_at": "2026-07-10T13:34:00Z",
      "notes": "CrossInk 1.4.0 stable firmware",
      "firmware_url": "https://github.com/uxjulia/CrossInk/releases/download/v1.4.0/firmware-tiny-v1.4.0.bin",
      "firmware_sha256": "8c3a15814966a3c89c347f7422292387b5fc97f78880dec3cc45b9eaba5746b1",
      "size": 5491200,
      "supported_devices": [
        "x4",
        "x3"
      ]
    },
    {
      "id": "stable-1.4.0-xlarge",
      "channel": "stable",
      "name": "1.4.0",
      "version": "1.4.0",
      "variant": "xlarge",
      "released_at": "2026-07-10T13:34:00Z",
      "notes": "CrossInk 1.4.0 stable firmware",
      "firmware_url": "https://github.com/uxjulia/CrossInk/releases/download/v1.4.0/firmware-xlarge-v1.4.0.bin",
      "firmware_sha256": "39dda1fbd5682691d9c91ec6c0bf2cf6448cc491fbc14f796d35829c2f386bd6",
      "size": 5354096,
      "supported_devices": [
        "x4",
        "x3"
      ]
    }
  ]
})";

void testRealCatalogTinyVariant() {
  printf("testRealCatalogTinyVariant...\n");

  CatalogJsonParser p("tiny");
  p.feed(kRealCatalog, strlen(kRealCatalog));

  ASSERT_TRUE(p.foundRelease());
  ASSERT_STREQ(p.getVersion(), "1.4.0");
  ASSERT_STREQ(p.getFirmwareUrl(),
               "https://github.com/uxjulia/CrossInk/releases/download/v1.4.0/firmware-tiny-v1.4.0.bin");
  ASSERT_STREQ(p.getFirmwareSha256(), "8c3a15814966a3c89c347f7422292387b5fc97f78880dec3cc45b9eaba5746b1");
  ASSERT_EQ(p.getFirmwareSize(), 5491200u);

  printf("  passed\n");
  PASS();
}

void testRealCatalogXlargeVariant() {
  printf("testRealCatalogXlargeVariant...\n");

  CatalogJsonParser p("xlarge");
  p.feed(kRealCatalog, strlen(kRealCatalog));

  ASSERT_TRUE(p.foundRelease());
  ASSERT_STREQ(p.getVersion(), "1.4.0");
  ASSERT_STREQ(p.getFirmwareUrl(),
               "https://github.com/uxjulia/CrossInk/releases/download/v1.4.0/firmware-xlarge-v1.4.0.bin");
  ASSERT_STREQ(p.getFirmwareSha256(), "39dda1fbd5682691d9c91ec6c0bf2cf6448cc491fbc14f796d35829c2f386bd6");
  ASSERT_EQ(p.getFirmwareSize(), 5354096u);

  printf("  passed\n");
  PASS();
}

void testUnknownVariantNotFound() {
  printf("testUnknownVariantNotFound...\n");

  CatalogJsonParser p("teensy");
  p.feed(kRealCatalog, strlen(kRealCatalog));

  ASSERT_TRUE(!p.foundRelease());

  printf("  passed\n");
  PASS();
}

void testNullVariantPicksFirstStable() {
  printf("testNullVariantPicksFirstStable...\n");

  CatalogJsonParser p(nullptr);
  p.feed(kRealCatalog, strlen(kRealCatalog));

  ASSERT_TRUE(p.foundRelease());
  ASSERT_STREQ(p.getVersion(), "1.4.0");
  ASSERT_EQ(p.getFirmwareSize(), 5491200u);

  printf("  passed\n");
  PASS();
}

void testChannelFilterSkipsNonStable() {
  printf("testChannelFilterSkipsNonStable...\n");

  const char* json = R"({
    "schema_version": 1,
    "releases": [
      {"channel": "beta", "version": "2.0.0-rc1", "variant": "tiny",
       "firmware_url": "https://example.com/rc.bin", "firmware_sha256": "aa", "size": 1},
      {"channel": "stable", "version": "1.9.0", "variant": "tiny",
       "firmware_url": "https://example.com/stable.bin", "firmware_sha256": "bb", "size": 2}
    ]
  })";

  CatalogJsonParser p("tiny");
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundRelease());
  ASSERT_STREQ(p.getVersion(), "1.9.0");
  ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/stable.bin");
  ASSERT_EQ(p.getFirmwareSize(), 2u);

  printf("  passed\n");
  PASS();
}

void testFirstMatchWins() {
  printf("testFirstMatchWins...\n");

  const char* json = R"({
    "releases": [
      {"channel": "stable", "version": "2.0.0", "variant": "tiny",
       "firmware_url": "https://example.com/first.bin", "size": 10},
      {"channel": "stable", "version": "1.0.0", "variant": "tiny",
       "firmware_url": "https://example.com/second.bin", "size": 20}
    ]
  })";

  CatalogJsonParser p("tiny");
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundRelease());
  ASSERT_STREQ(p.getVersion(), "2.0.0");
  ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/first.bin");

  printf("  passed\n");
  PASS();
}

void testEntryWithoutUrlSkipped() {
  printf("testEntryWithoutUrlSkipped...\n");

  const char* json = R"({
    "releases": [
      {"channel": "stable", "version": "2.0.0", "variant": "tiny", "size": 10},
      {"channel": "stable", "version": "1.5.0", "variant": "tiny",
       "firmware_url": "https://example.com/ok.bin", "size": 20}
    ]
  })";

  CatalogJsonParser p("tiny");
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundRelease());
  ASSERT_STREQ(p.getVersion(), "1.5.0");
  ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/ok.bin");

  printf("  passed\n");
  PASS();
}

void testEmptyReleases() {
  printf("testEmptyReleases...\n");

  const char* json = R"({"schema_version": 1, "releases": []})";

  CatalogJsonParser p("tiny");
  p.feed(json, strlen(json));

  ASSERT_TRUE(!p.foundRelease());

  printf("  passed\n");
  PASS();
}

void testMissingReleasesKey() {
  printf("testMissingReleasesKey...\n");

  const char* json = R"({"schema_version": 1})";

  CatalogJsonParser p("tiny");
  p.feed(json, strlen(json));

  ASSERT_TRUE(!p.foundRelease());

  printf("  passed\n");
  PASS();
}

void testTruncatedInsideRelease() {
  printf("testTruncatedInsideRelease...\n");

  const char* json = R"({"releases": [{"channel": "stable", "version": "1.4.0", "variant": "tiny", "firmware_)";

  CatalogJsonParser p("tiny");
  p.feed(json, strlen(json));

  ASSERT_TRUE(!p.foundRelease());

  printf("  passed\n");
  PASS();
}

void testNestedStructuresIgnored() {
  printf("testNestedStructuresIgnored...\n");

  // Nested objects/arrays inside a release must not confuse key tracking;
  // a "version" key inside a nested object must not leak into the release.
  const char* json = R"({
    "releases": [
      {"channel": "stable", "variant": "tiny",
       "metadata": {"version": "9.9.9", "extra": ["a", {"size": 999}]},
       "version": "1.4.0",
       "firmware_url": "https://example.com/fw.bin", "size": 42}
    ]
  })";

  CatalogJsonParser p("tiny");
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundRelease());
  ASSERT_STREQ(p.getVersion(), "1.4.0");
  ASSERT_EQ(p.getFirmwareSize(), 42u);

  printf("  passed\n");
  PASS();
}

void testResetAndReuse() {
  printf("testResetAndReuse...\n");

  CatalogJsonParser p("tiny");
  p.feed(kRealCatalog, strlen(kRealCatalog));
  ASSERT_TRUE(p.foundRelease());

  p.reset();
  ASSERT_TRUE(!p.foundRelease());
  ASSERT_STREQ(p.getVersion(), "");
  ASSERT_STREQ(p.getFirmwareUrl(), "");
  ASSERT_EQ(p.getFirmwareSize(), 0u);

  p.feed(kRealCatalog, strlen(kRealCatalog));
  ASSERT_TRUE(p.foundRelease());
  ASSERT_STREQ(p.getVersion(), "1.4.0");

  printf("  passed\n");
  PASS();
}

void testChunkedEveryBoundary() {
  printf("testChunkedEveryBoundary...\n");

  const char* json =
      R"({"releases":[{"channel":"stable","version":"1.4.0","variant":"tiny","firmware_url":"https://example.com/fw.bin","firmware_sha256":"8c3a15814966a3c89c347f7422292387b5fc97f78880dec3cc45b9eaba5746b1","size":5491200}]})";
  size_t len = strlen(json);

  for (size_t split = 0; split <= len; ++split) {
    CatalogJsonParser p("tiny");
    if (split > 0) p.feed(json, split);
    if (split < len) p.feed(json + split, len - split);

    ASSERT_TRUE(p.foundRelease());
    ASSERT_STREQ(p.getVersion(), "1.4.0");
    ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/fw.bin");
    ASSERT_STREQ(p.getFirmwareSha256(), "8c3a15814966a3c89c347f7422292387b5fc97f78880dec3cc45b9eaba5746b1");
    ASSERT_EQ(p.getFirmwareSize(), 5491200u);
  }

  printf("  passed (all %zu split points)\n", len + 1);
  PASS();
}

void testChunkedRealCatalogSmallChunks() {
  printf("testChunkedRealCatalogSmallChunks...\n");

  // Mimic esp_http_client delivering the body in small TLS-record fragments
  size_t len = strlen(kRealCatalog);
  for (size_t chunkSize = 1; chunkSize <= 7; ++chunkSize) {
    CatalogJsonParser p("xlarge");
    for (size_t offset = 0; offset < len; offset += chunkSize) {
      size_t n = offset + chunkSize <= len ? chunkSize : len - offset;
      p.feed(kRealCatalog + offset, n);
    }

    ASSERT_TRUE(p.foundRelease());
    ASSERT_STREQ(p.getVersion(), "1.4.0");
    ASSERT_EQ(p.getFirmwareSize(), 5354096u);
  }

  printf("  passed\n");
  PASS();
}

// ============================================================================

int main() {
  printf("=== CatalogJsonParser Tests ===\n\n");

  testRealCatalogTinyVariant();
  testRealCatalogXlargeVariant();
  testUnknownVariantNotFound();
  testNullVariantPicksFirstStable();
  testChannelFilterSkipsNonStable();
  testFirstMatchWins();
  testEntryWithoutUrlSkipped();
  testEmptyReleases();
  testMissingReleasesKey();
  testTruncatedInsideRelease();
  testNestedStructuresIgnored();
  testResetAndReuse();
  testChunkedEveryBoundary();
  testChunkedRealCatalogSmallChunks();

  printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
  return testsFailed > 0 ? 1 : 0;
}
