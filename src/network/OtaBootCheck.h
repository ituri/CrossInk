#pragma once

#include <cstddef>
#include <cstdint>

#include "OtaUpdater.h"

class GfxRenderer;

// Boot-time OTA stages. A TLS handshake to GitHub needs ~50KB of heap with a
// large contiguous block for the 16KB record buffer; once the activity stack
// and WiFi UI are up only ~40KB remains and checks fail with alloc errors or
// starved handshakes (#312). The OTA activity therefore records a request in
// RTC memory and silent-restarts; setup() runs the network work right after
// display init while >100KB is still free, then routes back into the activity
// with the result.
namespace OtaBootCheck {

enum class Stage : uint8_t { None = 0, Check, Install };

// One-shot outcome of a boot stage, consumed by OtaUpdateActivity.
struct Result {
  OtaUpdater::OtaUpdaterError error = OtaUpdater::HTTP_ERROR;
  char version[32] = {};
  char url[512] = {};
  char sha256[65] = {};
  uint32_t size = 0;
};

// Reads and clears the RTC request flag. Call exactly once per boot, before
// anything that could panic — a stale flag must not loop the boot.
Stage takeStage();

// Runs the requested stage: connects to the last saved WiFi network and either
// checks for an update or downloads and installs the one recorded by
// requestInstall(). Draws its own minimal status UI via the renderer. A
// successful install restarts into the new firmware and does not return.
void runStage(Stage stage, GfxRenderer& renderer);

// One-shot boot result for OtaUpdateActivity; nullptr when none is pending.
const Result* takeResult();

// True when a saved credential exists for the boot stage to connect with.
bool canAutoConnect();

// Record the request in RTC memory and silent-restart. On hardware these do
// not return; the simulator stubs fall through so callers must handle it.
void requestCheck();
void requestInstall(const char* version, const char* url, const char* sha256, size_t size);

}  // namespace OtaBootCheck
