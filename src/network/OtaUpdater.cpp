#ifdef SIMULATOR
#include "OtaUpdater.h"

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*, std::atomic<bool>*) { return NO_UPDATE; }
#else
#include <Arduino.h>
#include <CatalogJsonParser.h>
#include <Logging.h>
#include <Memory.h>
#include <ReleaseJsonParser.h>
#include <strings.h>

#include <cstring>

#include "AppVersion.h"
#include "OtaUpdater.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
#include "network/WifiPowerSaveGuard.h"

namespace {
#ifndef CROSSINK_OTA_RELEASE_URL
#define CROSSINK_OTA_RELEASE_URL "https://api.github.com/repos/uxjulia/CrossInk/releases/latest"
#endif

// Primary update manifest: the release catalog on GitHub Pages. Its ~1KB
// response with few small headers keeps esp_http_client's per-header heap
// allocations tiny, unlike the ~32KB api.github.com release JSON whose
// header/body parsing has crashed low-heap devices mid-TLS (#312).
#ifndef CROSSINK_OTA_CATALOG_URL
#define CROSSINK_OTA_CATALOG_URL "https://crossink.uxj.io/catalog"
#endif

constexpr char latestReleaseUrl[] = CROSSINK_OTA_RELEASE_URL;
constexpr char catalogUrl[] = CROSSINK_OTA_CATALOG_URL;

#ifdef CROSSPOINT_FIRMWARE_VARIANT
constexpr char firmwareAssetStem[] = "firmware-" CROSSPOINT_FIRMWARE_VARIANT;
constexpr char firmwareAssetName[] = "firmware-" CROSSPOINT_FIRMWARE_VARIANT ".bin";
constexpr const char* firmwareVariant = CROSSPOINT_FIRMWARE_VARIANT;
#else
constexpr char firmwareAssetStem[] = "firmware";
constexpr char firmwareAssetName[] = "firmware.bin";
constexpr const char* firmwareVariant = nullptr;
#endif

constexpr char binSuffix[] = ".bin";
constexpr size_t VERSION_SEGMENT_COUNT = 4;
constexpr size_t OTA_PROGRESS_UPDATE_BYTES = 64 * 1024;
constexpr int OTA_HTTP_READ_TIMEOUT_MS = 5000;
constexpr uint32_t OTA_DOWNLOAD_IDLE_TIMEOUT_MS = 30000;
constexpr size_t OTA_READ_BUFFER_SIZE = 1024;
constexpr uint8_t OTA_MAX_REDIRECTS = 5;

struct ParsedVersion {
  int segments[VERSION_SEGMENT_COUNT] = {0, 0, 0, 0};
  bool valid = false;
  bool releaseCandidate = false;
};

bool isDigit(const char c) { return c >= '0' && c <= '9'; }

bool startsWithNumberAfterOptionalV(const char* version) {
  if (version == nullptr) return false;
  if ((version[0] == 'v' || version[0] == 'V') && isDigit(version[1])) return true;
  return isDigit(version[0]);
}

bool containsRcMarker(const char* version) {
  if (version == nullptr) return false;
  for (const char* p = version; p[0] != '\0' && p[1] != '\0' && p[2] != '\0'; ++p) {
    if (p[0] == '-' && (p[1] == 'r' || p[1] == 'R') && (p[2] == 'c' || p[2] == 'C')) {
      return true;
    }
  }
  return false;
}

ParsedVersion parseVersion(const char* version) {
  ParsedVersion parsed;
  if (!startsWithNumberAfterOptionalV(version)) return parsed;

  const char* p = version;
  if (p[0] == 'v' || p[0] == 'V') ++p;

  size_t segmentIndex = 0;
  while (segmentIndex < VERSION_SEGMENT_COUNT) {
    if (!isDigit(*p)) return parsed;

    int value = 0;
    while (isDigit(*p)) {
      value = value * 10 + (*p - '0');
      ++p;
    }
    parsed.segments[segmentIndex] = value;
    ++segmentIndex;

    if (*p != '.') break;
    ++p;
  }

  parsed.valid = true;
  parsed.releaseCandidate = containsRcMarker(version);
  return parsed;
}

int compareVersions(const char* latestVersion, const char* currentVersion) {
  const ParsedVersion latest = parseVersion(latestVersion);
  const ParsedVersion current = parseVersion(currentVersion);
  if (!latest.valid || !current.valid) return 0;

  for (size_t i = 0; i < VERSION_SEGMENT_COUNT; ++i) {
    if (latest.segments[i] != current.segments[i]) {
      return latest.segments[i] > current.segments[i] ? 1 : -1;
    }
  }

  if (current.releaseCandidate && !latest.releaseCandidate) return 1;
  return 0;
}

bool startsWith(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) return false;
  const size_t prefixLength = strlen(prefix);
  return strncmp(value, prefix, prefixLength) == 0;
}

bool isRedirectStatus(const int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

esp_err_t captureLocationHeader(esp_http_client_event_t* evt) {
  auto* location = static_cast<std::string*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_HEADER && location != nullptr && evt->header_key != nullptr &&
      evt->header_value != nullptr && strcasecmp(evt->header_key, "Location") == 0) {
    location->assign(evt->header_value);
  }
  return ESP_OK;
}

struct ParsedUrl {
  bool https = false;
  std::string host;
  std::string path;
  uint16_t port = 80;
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;

  const std::string scheme = url.substr(0, schemeEnd);
  out.https = scheme == "https";
  if (!out.https && scheme != "http") return false;

  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  const std::string hostPort =
      url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
  out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  out.port = out.https ? 443 : 80;

  const size_t portSep = hostPort.rfind(':');
  if (portSep != std::string::npos) {
    out.host = hostPort.substr(0, portSep);
    const std::string portText = hostPort.substr(portSep + 1);
    if (portText.empty()) return false;
    uint32_t parsedPort = 0;
    for (const char c : portText) {
      if (c < '0' || c > '9') return false;
      parsedPort = parsedPort * 10 + static_cast<uint32_t>(c - '0');
      if (parsedPort > UINT16_MAX) return false;
    }
    if (parsedPort == 0) return false;
    out.port = static_cast<uint16_t>(parsedPort);
  } else {
    out.host = hostPort;
  }

  return !out.host.empty() && !out.path.empty();
}

std::string buildRedirectUrl(const std::string& baseUrl, const std::string& location) {
  if (startsWith(location.c_str(), "http://") || startsWith(location.c_str(), "https://")) return location;

  ParsedUrl base;
  if (!parseUrl(baseUrl, base)) return location;

  std::string origin = base.https ? "https://" : "http://";
  origin += base.host;
  if ((base.https && base.port != 443) || (!base.https && base.port != 80)) {
    origin += ":";
    origin += std::to_string(base.port);
  }

  if (!location.empty() && location[0] == '/') return origin + location;

  const size_t lastSlash = base.path.rfind('/');
  const std::string parent = lastSlash == std::string::npos ? "/" : base.path.substr(0, lastSlash + 1);
  return origin + parent + location;
}

char lowerHex(const uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

char asciiLower(const char c) { return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isHexChar(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool isSha256Hex(const char* value) {
  if (value == nullptr) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!isHexChar(value[i])) return false;
  }
  return value[64] == '\0';
}

bool sha256Matches(const uint8_t digest[32], const char* expectedHex) {
  if (!isSha256Hex(expectedHex)) return false;

  for (size_t i = 0; i < 32; ++i) {
    const char high = lowerHex((digest[i] >> 4) & 0x0F);
    const char low = lowerHex(digest[i] & 0x0F);
    if (high != asciiLower(expectedHex[i * 2]) || low != asciiLower(expectedHex[i * 2 + 1])) return false;
  }
  return true;
}

void formatSha256(const uint8_t digest[32], char output[65]) {
  for (size_t i = 0; i < 32; ++i) {
    output[i * 2] = lowerHex((digest[i] >> 4) & 0x0F);
    output[i * 2 + 1] = lowerHex(digest[i] & 0x0F);
  }
  output[64] = '\0';
}

bool isHttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0; }

bool endsWith(const char* value, const char* suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t valueLength = strlen(value);
  const size_t suffixLength = strlen(suffix);
  if (suffixLength > valueLength) return false;
  return strcmp(value + valueLength - suffixLength, suffix) == 0;
}

bool isMatchingFirmwareAssetName(const char* assetName) {
  if (assetName == nullptr) return false;
  if (strcmp(assetName, firmwareAssetName) == 0) return true;
  if (!startsWith(assetName, firmwareAssetStem)) return false;
  if (assetName[strlen(firmwareAssetStem)] != '-') return false;
  return endsWith(assetName, binSuffix);
}

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);
}

size_t totalBytesReceived = 0;

struct OtaInstallContext {
  size_t* processedSize = nullptr;
  size_t totalSize = 0;
  size_t lastProgressBytes = 0;
  int lastReportedPct = -1;
  OtaUpdater::ProgressCallback onProgress = nullptr;
  void* progressCtx = nullptr;
};

void logTlsError(esp_http_client_handle_t client, const char* phase) {
  int tlsError = 0;
  int tlsFlags = 0;
  const esp_err_t err = esp_http_client_get_and_clear_last_tls_error(client, &tlsError, &tlsFlags);
  if (err != ESP_OK || tlsError != 0 || tlsFlags != 0) {
    const int tlsCode = tlsError < 0 ? -tlsError : tlsError;
    LOG_ERR("OTA", "%s TLS error: err=%s mbedtls=0x%x flags=0x%x", phase, esp_err_to_name(err), tlsCode, tlsFlags);
  }
}

// Type-erased parser feed so the same HTTP fetch works for the catalog and
// the GitHub release JSON without std::function.
struct ManifestFeed {
  void (*feed)(void* parser, const char* data, size_t len) = nullptr;
  void (*reset)(void* parser) = nullptr;
  void* parser = nullptr;
};

esp_err_t manifest_event_handler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  if (event->data_len <= 0) return ESP_OK;

  auto* feed = static_cast<ManifestFeed*>(event->user_data);
  if (feed == nullptr || feed->feed == nullptr) {
    LOG_ERR("OTA", "HTTP client parser missing");
    return ESP_ERR_INVALID_ARG;
  }

  totalBytesReceived += static_cast<size_t>(event->data_len);
  LOG_DBG("OTA", "HTTP chunk: %d bytes (total: %zu)", event->data_len, totalBytesReceived);
  feed->feed(feed->parser, static_cast<const char*>(event->data), static_cast<size_t>(event->data_len));
  return ESP_OK;
}

OtaUpdater::OtaUpdaterError fetchManifestOnce(const char* url, ManifestFeed& feed, const int bufferSize) {
  esp_http_client_config_t client_config = {
      .url = url,
      // The 5s default cuts off handshakes that stall while lwIP waits for
      // retransmits under heap pressure; match installUpdate's 15s budget.
      .timeout_ms = 15000,
      .event_handler = manifest_event_handler,
      // Sized per manifest: the GitHub API needs 4096 for its headers, the
      // Pages catalog fits in 2048. The body streams through the parser in
      // chunks so RX needn't be larger. TX only carries our GET.
      .buffer_size = bufferSize,
      .buffer_size_tx = 1024,
      .user_data = &feed,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  totalBytesReceived = 0;

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return OtaUpdater::INTERNAL_UPDATE_ERROR;
  }

  esp_err_t esp_err = esp_http_client_set_header(client_handle, "User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return OtaUpdater::INTERNAL_UPDATE_ERROR;
  }

  // Measured here, not in the failure path: by the time an error is logged
  // the TLS handle is already freed and the numbers no longer show what the
  // handshake actually had to work with (#312 diagnostics).
  LOG_INF("OTA", "Fetching manifest (heap=%u maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s (heap=%u maxAlloc=%u minFree=%u)", esp_err_to_name(esp_err),
            ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    logTlsError(client_handle, "Manifest fetch failure");
    esp_http_client_cleanup(client_handle);
    return OtaUpdater::HTTP_ERROR;
  }

  const int statusCode = esp_http_client_get_status_code(client_handle);

  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return OtaUpdater::INTERNAL_UPDATE_ERROR;
  }

  if (statusCode < 200 || statusCode >= 300) {
    LOG_ERR("OTA", "Manifest HTTP status: %d", statusCode);
    return OtaUpdater::HTTP_ERROR;
  }

  LOG_DBG("OTA", "Response received: %zu bytes total", totalBytesReceived);
  return OtaUpdater::OK;
}

// A failed TLS attempt leaves the heap measurably less fragmented once its
// buffers are freed (field logs in #312 show maxAlloc recovering right after
// a failure), so one retry often succeeds where the first attempt could not
// allocate the 16KB TLS record buffer.
OtaUpdater::OtaUpdaterError fetchManifest(const char* url, ManifestFeed& feed, const int bufferSize) {
  OtaUpdater::OtaUpdaterError result = fetchManifestOnce(url, feed, bufferSize);
  if (result == OtaUpdater::OK) return result;

  LOG_INF("OTA", "Retrying manifest fetch");
  delay(250);
  if (feed.reset != nullptr) feed.reset(feed.parser);
  return fetchManifestOnce(url, feed, bufferSize);
}

void notifyOtaProgress(OtaInstallContext* ctx, const bool force) {
  if (ctx == nullptr || ctx->onProgress == nullptr || ctx->processedSize == nullptr || ctx->totalSize == 0) return;

  const size_t processed = *ctx->processedSize;
  const int pct = static_cast<int>(static_cast<uint64_t>(processed) * 100 / ctx->totalSize);
  if (force || pct != ctx->lastReportedPct || processed - ctx->lastProgressBytes >= OTA_PROGRESS_UPDATE_BYTES) {
    ctx->lastReportedPct = pct;
    ctx->lastProgressBytes = processed;
    ctx->onProgress(ctx->progressCtx);
  }
}

}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  WifiPowerSaveGuard wifiPowerSaveGuard;

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSha256.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  LOG_DBG("OTA", "Checking for update (current: %s, heap=%u maxAlloc=%u)", CROSSINK_VERSION, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  // Catalog first: tiny response, cheap to parse. Fall back to the GitHub
  // releases API so OTA keeps working if Pages/catalog is unavailable.
  {
    CatalogJsonParser catalogParser(firmwareVariant);
    ManifestFeed feed{
        [](void* parser, const char* data, size_t len) { static_cast<CatalogJsonParser*>(parser)->feed(data, len); },
        [](void* parser) { static_cast<CatalogJsonParser*>(parser)->reset(); }, &catalogParser};
    const OtaUpdaterError catalogResult = fetchManifest(catalogUrl, feed, 2048);
    if (catalogResult == OK && catalogParser.foundRelease()) {
      latestVersion = catalogParser.getVersion();
      otaUrl = catalogParser.getFirmwareUrl();
      otaSha256 = catalogParser.getFirmwareSha256();
      otaSize = catalogParser.getFirmwareSize();
      totalSize = otaSize;
      updateAvailable = true;

      LOG_DBG("OTA", "Catalog update: version=%s size=%zu sha256=%s", latestVersion.c_str(), otaSize,
              otaSha256.empty() ? "missing" : "present");
      LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
      return OK;
    }
    LOG_ERR("OTA", "Catalog check failed (result=%d found=%d), falling back to GitHub API", catalogResult,
            catalogParser.foundRelease() ? 1 : 0);
  }

  ReleaseJsonParser releaseParser(isMatchingFirmwareAssetName);
  ManifestFeed feed{
      [](void* parser, const char* data, size_t len) { static_cast<ReleaseJsonParser*>(parser)->feed(data, len); },
      [](void* parser) { static_cast<ReleaseJsonParser*>(parser)->reset(); }, &releaseParser};
  const OtaUpdaterError releaseResult = fetchManifest(latestReleaseUrl, feed, 4096);
  if (releaseResult != OK) {
    return releaseResult;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  latestVersion = releaseParser.getTagName();

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No matching %s asset found for release %s", firmwareAssetStem, latestVersion.c_str());
    return NO_UPDATE;
  }

  // Prefer the api.github.com asset endpoint: with Accept:octet-stream it
  // redirects straight to the CDN, skipping github.com's web tier whose ~5KB
  // response headers exhaust low-heap devices mid-parse (#312).
  otaUrl =
      releaseParser.getFirmwareApiUrl()[0] != '\0' ? releaseParser.getFirmwareApiUrl() : releaseParser.getFirmwareUrl();
  otaSha256 = releaseParser.getFirmwareSha256();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu sha256=%s", latestVersion.c_str(), otaSize,
          otaSha256.empty() ? "missing" : "present");
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSINK_VERSION) {
    return false;
  }

  const int comparison = compareVersions(latestVersion.c_str(), CROSSINK_VERSION);
  LOG_DBG("OTA", "Version comparison latest=%s current=%s result=%d", latestVersion.c_str(), CROSSINK_VERSION,
          comparison);
  return comparison > 0;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx,
                                                      std::atomic<bool>* cancelRequested) {
  const auto isCancellationRequested = [cancelRequested]() -> bool {
    return cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed);
  };

  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  if (isCancellationRequested()) {
    return CANCELLED_ERROR;
  }
  if (isHttpUrl(otaUrl) && !isSha256Hex(otaSha256.c_str())) {
    LOG_ERR("OTA", "Refusing HTTP firmware URL without manifest sha256");
    return JSON_PARSE_ERROR;
  }

  processedSize = 0;

  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    LOG_ERR("OTA", "No OTA update partition found");
    return INTERNAL_UPDATE_ERROR;
  }

  if (otaSize > 0 && otaSize > updatePartition->size) {
    LOG_ERR("OTA", "Firmware too large: %zu > %zu", otaSize, updatePartition->size);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  OtaInstallContext installCtx;
  installCtx.processedSize = &processedSize;
  installCtx.totalSize = totalSize;
  installCtx.onProgress = onProgress;
  installCtx.progressCtx = ctx;

  WifiPowerSaveGuard wifiPowerSaveGuard;

  LOG_INF("OTA", "Starting firmware download: url=%s heap=%u maxAlloc=%u", otaUrl.c_str(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  std::string currentUrl = otaUrl;
  esp_http_client_handle_t client = nullptr;
  int64_t contentLength = -1;
  int statusCode = 0;
  esp_err_t esp_err = ESP_OK;

  for (uint8_t hop = 0; hop < OTA_MAX_REDIRECTS; ++hop) {
    std::string redirectLocation;
    esp_http_client_config_t client_config = {};
    client_config.url = currentUrl.c_str();
    client_config.timeout_ms = 15000;
    // Hop 0 hits github.com whose 302 carries ~5KB of headers (a 3.6KB CSP
    // alone), so it needs the 4KB header buffer. The post-redirect CDN sends
    // <1KB of headers; every KB freed here raises the heap floor under the
    // TLS handshake (#312).
    client_config.buffer_size = hop == 0 ? 4096 : 1536;
    client_config.buffer_size_tx = 1024;
    client_config.skip_cert_common_name_check = true;
    // Signed-artifact model, like apt: hop 0 talks to the manifest authority
    // and is always chain-verified; it supplied the firmware sha256 that
    // installUpdate enforces below, so the CDN hops the authority redirects
    // to add no integrity and skip chain verification when that sha256 is
    // present. A tampered download fails HASH_MISMATCH_ERROR and is never
    // booted. This matters on the ESP32-C3: the CDN's Let's Encrypt chain
    // needs software RSA-4096 verifies whose handshake peak (~65KB) exceeds
    // any heap this firmware can free up (#312).
    if (hop == 0 || !isSha256Hex(otaSha256.c_str())) {
      client_config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    client_config.event_handler = captureLocationHeader;
    client_config.user_data = &redirectLocation;
    client_config.keep_alive_enable = false;
    client_config.disable_auto_redirect = true;

    client = esp_http_client_init(&client_config);
    if (client == nullptr) {
      LOG_ERR("OTA", "HTTP client init failed (heap=%u maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      return HTTP_ERROR;
    }

    esp_err = http_client_set_header_cb(client);
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "Failed to set OTA User-Agent: %s", esp_err_to_name(esp_err));
      esp_http_client_cleanup(client);
      return INTERNAL_UPDATE_ERROR;
    }

    // Makes the api.github.com asset endpoint 302 to the CDN instead of
    // returning JSON; ignored by the other hosts on the redirect chain.
    esp_err = esp_http_client_set_header(client, "Accept", "application/octet-stream");
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "Failed to set OTA Accept header: %s", esp_err_to_name(esp_err));
      esp_http_client_cleanup(client);
      return INTERNAL_UPDATE_ERROR;
    }

    LOG_INF("OTA", "Opening firmware connection");
    esp_err = esp_http_client_open(client, 0);
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "Firmware HTTP open failed: %s (heap=%u maxAlloc=%u minFree=%u)", esp_err_to_name(esp_err),
              ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
      logTlsError(client, "Firmware open failure");
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }

    LOG_INF("OTA", "Fetching firmware headers (heap=%u maxAlloc=%u minFree=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    contentLength = esp_http_client_fetch_headers(client);
    statusCode = esp_http_client_get_status_code(client);
    LOG_INF("OTA", "Firmware headers done: status=%d len=%lld (heap=%u maxAlloc=%u minFree=%u)", statusCode,
            static_cast<long long>(contentLength), ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    if (contentLength < 0) {
      LOG_ERR("OTA", "Firmware header fetch failed: %lld", static_cast<long long>(contentLength));
      logTlsError(client, "Firmware header failure");
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }
    if (!isRedirectStatus(statusCode)) {
      break;
    }

    if (redirectLocation.empty()) {
      LOG_ERR("OTA", "Firmware redirect missing Location header");
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }

    const std::string redirectUrl = buildRedirectUrl(currentUrl, redirectLocation);
    ParsedUrl currentParsed;
    ParsedUrl redirectParsed;
    if (!parseUrl(redirectUrl, redirectParsed)) {
      LOG_ERR("OTA", "Rejected firmware redirect with unsupported Location");
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }
    if (parseUrl(currentUrl, currentParsed) && currentParsed.https && !redirectParsed.https) {
      LOG_ERR("OTA", "Rejected firmware HTTPS downgrade redirect to %s", redirectParsed.host.c_str());
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }

    LOG_DBG("OTA", "Following firmware redirect to %s", redirectParsed.host.c_str());
    esp_http_client_cleanup(client);
    client = nullptr;
    currentUrl = redirectUrl;
  }

  if (client == nullptr) {
    LOG_ERR("OTA", "Firmware redirect limit exceeded");
    return HTTP_ERROR;
  }
  if (statusCode < 200 || statusCode >= 300) {
    LOG_ERR("OTA", "Firmware HTTP status: %d", statusCode);
    esp_http_client_cleanup(client);
    return HTTP_ERROR;
  }

  const size_t firmwareSize = contentLength > 0 ? static_cast<size_t>(contentLength) : otaSize;
  if (firmwareSize > 0) {
    if (firmwareSize > updatePartition->size) {
      LOG_ERR("OTA", "Firmware response too large: %zu > %zu", firmwareSize, updatePartition->size);
      esp_http_client_cleanup(client);
      return INTERNAL_UPDATE_ERROR;
    }
    totalSize = firmwareSize;
    installCtx.totalSize = firmwareSize;
  }

  LOG_INF("OTA", "Writing firmware to %s @0x%x size=%zu heap=%u maxAlloc=%u", updatePartition->label,
          static_cast<unsigned>(updatePartition->address), firmwareSize, ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  esp_err = esp_ota_begin(updatePartition, firmwareSize > 0 ? firmwareSize : OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s (heap=%u maxAlloc=%u)", esp_err_to_name(esp_err), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    esp_http_client_cleanup(client);
    return esp_err == ESP_ERR_NO_MEM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_timeout_ms(client, OTA_HTTP_READ_TIMEOUT_MS);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "Failed to set OTA read timeout: %s", esp_err_to_name(esp_err));
    esp_ota_abort(otaHandle);
    esp_http_client_cleanup(client);
    return INTERNAL_UPDATE_ERROR;
  }

  // Allocated only now: during the redirect/TLS phase above every free KB
  // counts, and the buffer is first needed for the read loop below.
  auto buffer = makeUniqueNoThrow<char[]>(OTA_READ_BUFFER_SIZE);
  if (!buffer) {
    LOG_ERR("OTA", "Failed to allocate %zu byte OTA read buffer (heap=%u maxAlloc=%u)", OTA_READ_BUFFER_SIZE,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    esp_ota_abort(otaHandle);
    esp_http_client_cleanup(client);
    return OOM_ERROR;
  }

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);

  uint32_t lastReadMs = millis();
  while (true) {
    if (isCancellationRequested()) {
      LOG_INF("OTA", "Update cancelled");
      mbedtls_sha256_free(&shaCtx);
      esp_ota_abort(otaHandle);
      esp_http_client_cleanup(client);
      return CANCELLED_ERROR;
    }

    const int bytesRead = esp_http_client_read(client, buffer.get(), OTA_READ_BUFFER_SIZE);
    if (bytesRead < 0) {
      if (bytesRead == -ESP_ERR_HTTP_EAGAIN) {
        const uint32_t idleMs = millis() - lastReadMs;
        if (idleMs >= OTA_DOWNLOAD_IDLE_TIMEOUT_MS) {
          LOG_ERR("OTA", "Firmware read timed out after %zu/%zu bytes (idle=%lu ms)", processedSize, totalSize,
                  static_cast<unsigned long>(idleMs));
          mbedtls_sha256_free(&shaCtx);
          esp_ota_abort(otaHandle);
          esp_http_client_cleanup(client);
          return HTTP_ERROR;
        }
        delay(1);
        continue;
      }

      LOG_ERR("OTA", "Firmware read failed after %zu/%zu bytes", processedSize, totalSize);
      logTlsError(client, "Firmware read failure");
      mbedtls_sha256_free(&shaCtx);
      esp_ota_abort(otaHandle);
      esp_http_client_cleanup(client);
      return HTTP_ERROR;
    }
    if (bytesRead == 0) break;

    esp_err = esp_ota_write(otaHandle, buffer.get(), static_cast<size_t>(bytesRead));
    if (esp_err != ESP_OK) {
      LOG_ERR("OTA", "esp_ota_write failed after %zu bytes: %s", processedSize, esp_err_to_name(esp_err));
      mbedtls_sha256_free(&shaCtx);
      esp_ota_abort(otaHandle);
      esp_http_client_cleanup(client);
      return INTERNAL_UPDATE_ERROR;
    }

    mbedtls_sha256_update(&shaCtx, reinterpret_cast<const unsigned char*>(buffer.get()),
                          static_cast<size_t>(bytesRead));
    processedSize += static_cast<size_t>(bytesRead);
    lastReadMs = millis();
    notifyOtaProgress(&installCtx, false);
    if (totalSize > 0 && processedSize >= totalSize) break;
    delay(0);
  }

  if (isCancellationRequested()) {
    LOG_INF("OTA", "Update cancelled");
    mbedtls_sha256_free(&shaCtx);
    esp_ota_abort(otaHandle);
    esp_http_client_cleanup(client);
    return CANCELLED_ERROR;
  }

  if (!esp_http_client_is_complete_data_received(client)) {
    LOG_ERR("OTA", "Firmware download incomplete: %zu/%zu", processedSize, totalSize);
    mbedtls_sha256_free(&shaCtx);
    esp_ota_abort(otaHandle);
    esp_http_client_cleanup(client);
    return INTERNAL_UPDATE_ERROR;
  }
  esp_http_client_cleanup(client);

  notifyOtaProgress(&installCtx, true);

  uint8_t computedSha256[32];
  mbedtls_sha256_finish(&shaCtx, computedSha256);
  mbedtls_sha256_free(&shaCtx);
  if (!otaSha256.empty()) {
    if (!sha256Matches(computedSha256, otaSha256.c_str())) {
      char computedSha256Hex[65];
      formatSha256(computedSha256, computedSha256Hex);
      LOG_ERR("OTA", "Firmware sha256 mismatch: expected=%s actual=%s", otaSha256.c_str(), computedSha256Hex);
      esp_ota_abort(otaHandle);
      return HASH_MISMATCH_ERROR;
    }
    LOG_INF("OTA", "Firmware sha256 verified");
  }

  esp_err = esp_ota_end(otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed: %zu bytes", processedSize);
  return OK;
}
#endif
