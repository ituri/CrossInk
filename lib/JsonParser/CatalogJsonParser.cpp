#include "CatalogJsonParser.h"

#include <cstdlib>
#include <cstring>

namespace {

void safeCopy(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace

CatalogJsonParser::CatalogJsonParser(const char* variant, const char* channel)
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}),
      wantedVariant(variant),
      wantedChannel(channel) {
  reset();
}

void CatalogJsonParser::reset() {
  parser.reset();
  position = Position::TOP_LEVEL;
  lastKey = LastKey::NONE;
  depth = 0;
  releaseDepth = 0;
  version[0] = '\0';
  firmwareUrl[0] = '\0';
  firmwareSha256[0] = '\0';
  firmwareSize = 0;
  releaseFound = false;
  currentChannel[0] = '\0';
  currentVersion[0] = '\0';
  currentVariant[0] = '\0';
  currentUrl[0] = '\0';
  currentSha256[0] = '\0';
  currentSize = 0;
}

void CatalogJsonParser::feed(const char* data, size_t len) { parser.feed(data, len); }

bool CatalogJsonParser::foundRelease() const { return releaseFound; }
const char* CatalogJsonParser::getVersion() const { return version; }
const char* CatalogJsonParser::getFirmwareUrl() const { return firmwareUrl; }
const char* CatalogJsonParser::getFirmwareSha256() const { return firmwareSha256; }
size_t CatalogJsonParser::getFirmwareSize() const { return firmwareSize; }

void CatalogJsonParser::commitRelease() {
  const bool variantMatches = wantedVariant == nullptr || strcmp(currentVariant, wantedVariant) == 0;
  const bool channelMatches = wantedChannel == nullptr || strcmp(currentChannel, wantedChannel) == 0;
  if (!releaseFound && variantMatches && channelMatches && currentVersion[0] != '\0' && currentUrl[0] != '\0') {
    memcpy(version, currentVersion, sizeof(version));
    memcpy(firmwareUrl, currentUrl, sizeof(firmwareUrl));
    memcpy(firmwareSha256, currentSha256, sizeof(firmwareSha256));
    firmwareSize = currentSize;
    releaseFound = true;
  }
  currentChannel[0] = '\0';
  currentVersion[0] = '\0';
  currentVariant[0] = '\0';
  currentUrl[0] = '\0';
  currentSha256[0] = '\0';
  currentSize = 0;
}

// -- SAX callbacks (static trampolines) -------------------------------------

void CatalogJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<CatalogJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth == 1) {
        if (len == 8 && memcmp(key, "releases", 8) == 0)
          self->lastKey = LastKey::RELEASES;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    case Position::IN_RELEASE_OBJECT:
      if (self->releaseDepth == 1) {
        if (len == 7 && memcmp(key, "channel", 7) == 0)
          self->lastKey = LastKey::CHANNEL;
        else if (len == 7 && memcmp(key, "version", 7) == 0)
          self->lastKey = LastKey::VERSION;
        else if (len == 7 && memcmp(key, "variant", 7) == 0)
          self->lastKey = LastKey::VARIANT;
        else if (len == 12 && memcmp(key, "firmware_url", 12) == 0)
          self->lastKey = LastKey::FIRMWARE_URL;
        else if (len == 15 && memcmp(key, "firmware_sha256", 15) == 0)
          self->lastKey = LastKey::FIRMWARE_SHA256;
        else if (len == 4 && memcmp(key, "size", 4) == 0)
          self->lastKey = LastKey::SIZE;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    default:
      break;
  }
}

void CatalogJsonParser::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<CatalogJsonParser*>(ctx);

  if (self->position == Position::IN_RELEASE_OBJECT && self->releaseDepth == 1) {
    switch (self->lastKey) {
      case LastKey::CHANNEL:
        safeCopy(self->currentChannel, sizeof(self->currentChannel), value, len);
        break;
      case LastKey::VERSION:
        safeCopy(self->currentVersion, sizeof(self->currentVersion), value, len);
        break;
      case LastKey::VARIANT:
        safeCopy(self->currentVariant, sizeof(self->currentVariant), value, len);
        break;
      case LastKey::FIRMWARE_URL:
        safeCopy(self->currentUrl, sizeof(self->currentUrl), value, len);
        break;
      case LastKey::FIRMWARE_SHA256:
        safeCopy(self->currentSha256, sizeof(self->currentSha256), value, len);
        break;
      default:
        break;
    }
  }
  self->lastKey = LastKey::NONE;
}

void CatalogJsonParser::sOnNumber(void* ctx, const char* value, size_t /*len*/) {
  auto* self = static_cast<CatalogJsonParser*>(ctx);

  if (self->lastKey == LastKey::SIZE && self->position == Position::IN_RELEASE_OBJECT && self->releaseDepth == 1) {
    self->currentSize = static_cast<size_t>(strtoul(value, nullptr, 10));
  }
  self->lastKey = LastKey::NONE;
}

void CatalogJsonParser::sOnBool(void* ctx, bool /*value*/) {
  static_cast<CatalogJsonParser*>(ctx)->lastKey = LastKey::NONE;
}

void CatalogJsonParser::sOnNull(void* ctx) { static_cast<CatalogJsonParser*>(ctx)->lastKey = LastKey::NONE; }

void CatalogJsonParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<CatalogJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      self->depth++;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_RELEASES_ARRAY:
      self->position = Position::IN_RELEASE_OBJECT;
      self->releaseDepth = 1;
      self->currentChannel[0] = '\0';
      self->currentVersion[0] = '\0';
      self->currentVariant[0] = '\0';
      self->currentUrl[0] = '\0';
      self->currentSha256[0] = '\0';
      self->currentSize = 0;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_RELEASE_OBJECT:
      self->releaseDepth++;
      self->lastKey = LastKey::NONE;
      break;
  }
}

void CatalogJsonParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<CatalogJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_RELEASE_OBJECT:
      self->releaseDepth--;
      if (self->releaseDepth == 0) {
        self->commitRelease();
        self->position = Position::IN_RELEASES_ARRAY;
      }
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void CatalogJsonParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<CatalogJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->lastKey == LastKey::RELEASES && self->depth == 1) {
        self->position = Position::IN_RELEASES_ARRAY;
      } else {
        self->depth++;
      }
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_RELEASE_OBJECT:
      self->releaseDepth++;
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void CatalogJsonParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<CatalogJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_RELEASES_ARRAY:
      self->position = Position::TOP_LEVEL;
      break;
    case Position::IN_RELEASE_OBJECT:
      self->releaseDepth--;
      self->lastKey = LastKey::NONE;
      break;
  }
}
