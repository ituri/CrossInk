# CrossPoint Reader — Durable Context

Keep this file focused on repo-specific gotchas that are worth reusing in future sessions.

## Simulator

- Simulator patches belong in the adjacent `crosspoint-simulator` repo.
- The valid local simulator env in this repo is `simulator`, and `pio run -e simulator` currently builds cleanly.
- The simulator `PNGdec` stub in `crosspoint-simulator/src/PNGdec.h` needs to mirror the real API shape used by app code, including `hasAlpha()` and `getTransparentColor()`, even though decode still fails intentionally.
- Known simulator limits:
  - No image rendering: `platformio.ini` ignores `hal`, `PNGdec`, and `JPEGDEC`, so image decoders are intentionally absent.
  - JPEGDEC stub always fails; `JPEGDEC fallback: open failed (err=-1)` is expected in simulator.
  - `esp_deep_sleep_start()` is a no-op in simulator.
  - `HalStorage` uses POSIX file access under `./fs_` and allows multiple readers, unlike real hardware.

## Real Hardware / Storage

- SdFat on hardware allows only one open reader per file path at a time. If a fallback needs to reopen the same file, close the first handle before reopening.

## Rendering / Reader Pipeline

- `lib/Epub/Epub/Page.cpp`: images must render only in `GfxRenderer::BW`; grayscale passes are text anti-aliasing passes only.
- Kindle EPUBs may contain paired high-res and old-Kindle fallback images. `ChapterHtmlSlimParser` should skip `<img>` nodes with `data-AmznRemoved-M8` to avoid duplicate stacked images.
- After image/layout pipeline changes that affect cached EPUB output, clear the affected `.crosspoint/epub_<hash>/` cache if behavior looks stale.

## Misc Repo Gotchas

- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`: `"UTC-1"` means UTC+1.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()`, so header changes in the base theme must be duplicated in Lyra if needed.

## Build Tooling

- A system-Python upgrade breaks PlatformIO's venv (`ModuleNotFoundError: platformio`); fix with `python3 -m venv --clear ~/.platformio/penv && ~/.platformio/penv/bin/pip install platformio`.
- `freeink-sdk` is a git submodule; `git submodule update --init --recursive` is required before the first firmware build.
- `custom_sdkconfig` in `platformio.ini` triggers a pioarduino hybrid rebuild of the Arduino core (full IDF compile, ~10+ min first time). Components with EMBED_TXTFILES certs (esp_insights, esp_rainmaker) break that rebuild and are removed via `custom_component_remove`; `esp-dsp` must stay because PNGdec's `s3_simd_rgb565.S` includes `dsps_fft2r_platform.h`.
- `custom_component_remove` mutates `idf_component.yml` inside the installed `framework-arduinoespressif32` package, and the hybrid state marker is a generated `sdkconfig.defaults` in the repo root (gitignored). After changing the remove list **or** `custom_sdkconfig`, reset with: `rm -rf ~/.platformio/packages/framework-arduinoespressif32 ~/.platformio/packages/framework-arduinoespressif32-libs .pio/build/<env> sdkconfig.defaults sdkconfig.<env>` — incremental hybrid rebuilds otherwise leave stale libs (e.g. `esp-tls` rebuilt with `MBEDTLS_DYNAMIC_BUFFER` but `libmbedtls` without its `port/dynamic` sources → undefined `esp_mbedtls_dynamic_set_rx_buf_static` at link).
