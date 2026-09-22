# Engineering notes

Non-obvious decisions and gotchas for the CommandAGI ESP32-CAM firmware. The [README](README.md) is
the how-to; this is the _why_ (and the traps we already hit).

## Boot order: BLE before camera before mic

`setup()` brings up NimBLE **first**. The NimBLE controller needs internal (DMA-capable) RAM for its
mutexes/queues, and `esp_camera_init` grabs a big chunk of the same RAM. Initialize the camera first
and NimBLE asserts on a null mutex handle and boot-loops. So: `BleProv::begin()` → `Camera::begin()`
→ `Audio::begin()`. Wiring the STATUS notifier before the camera also means a camera failure still
notifies the app instead of dying silently.

## Camera and mic are independent

Each sensor is probed at boot (`g_cameraOk`, `g_audioOk`) and the main loop streams whatever is
present: camera-only, mic-only, or both. A missing/failed sensor just drops out — never blocks the
other. This is the same "if available" contract the phone/desktop hosts use for their sensors.

## I2S pins (INMP441)

GPIO13/14/15 are the AI-Thinker's HS2 **SD-card** pins — free because we don't use a microSD. Avoid
GPIO16 (PSRAM) and the strapping pin GPIO12. The INMP441 is read as a 32-bit slot (24 valid bits),
left channel (L/R → GND); we take the top 16 bits with `CAGI_AUDIO_SHIFT` as the gain. Buffers live
in PSRAM so a 1 s 16 kHz clip (~32 KB) doesn't starve internal RAM. Override the pins for ESP32-S3 in
`config.h`.

## Wi-Fi + BLE coexistence

The classic ESP32 shares one radio between Wi-Fi and BLE. The coexistence layer **requires** Wi-Fi
modem sleep when BLE is also active — `WiFi.setSleep(true)`. Disabling it aborts with "Should enable
WiFi modem sleep when both WiFi and Bluetooth are enabled".

## Remote sensor control without a socket

The camera is an HTTP poller, not a WebSocket host. It learns the operator's desired cam/mic state
two ways: (1) piggybacked on every frame/audio POST **response** (`{ sensors: { cam, mic } }`) while
streaming, so a toggle takes effect within one interval; (2) a `GET …/control` poll every few seconds
while `paused` (all sensors off), so a fully-stopped camera can still be turned back on remotely. A
missing key defaults to ON, preserving "stream everything" for a fresh unit.

## Per-unit PIN

The provisioning PIN seals the BLE payload (Wi-Fi password + account key). The reference build bakes
`123456`; production units must each get a unique PIN — see [`tools/batch-flash.mjs`](tools/README.md),
which mints one per board, compiles it in, and prints a label that matches the MAC suffix the app
shows.

**Prefer the monorepo-level general flasher.** The camera-only `tools/batch-flash.mjs`
(`make firmware-batch`) still works, but the canonical way to flash/label any product (camera, arm,
future) is now the repo-root `scripts/flash-device.mjs` / `make flash-device PRODUCT=camera` — same
PIN-mint + suffix-read + `labels.csv` flow, one tool across products (see the repo `scripts/README.md`).

## Always re-flashable

No Secure Boot / Flash Encryption / download-disable eFuse, ever. Serial download mode lives in mask
ROM, so GPIO0→GND + reset always recovers a board regardless of firmware state. `factory-reset` wipes
only the `cagi` NVS namespace, never the app. See README → _Why it can ALWAYS be re-flashed_.

## Verified SKU: manifest is EVIDENCE, not authority

The verified build (`-DCAGI_VERIFIED_SKU`, env `esp32cam-verified`) signs a `CaptureManifest`
(`src/manifest.cpp`, mirroring `packages/domain/core/src/integrity.ts`) each ~2 s and streams it as a JSON
sidecar over the same frame socket (`CloudWs::sendManifest`). It carries what the device claims about
ITSELF — firmware, boot state, the tamper switch's `caseIntact`, and which sensor modalities are live —
signed so the claim can't be forged in transit. But the device does **not** get to set its own
integrity score: the platform re-verifies the signature and **recomputes** cross-modal coherence from
the actual frames (`capture-coherence.ts`). `selfCoherence` in the manifest is only a hint (we emit a
neutral `1`). The signing message pre-image is byte-identical to `manifestSigningMessage()`
(`cagi-manifest:v1|seq|fw|bootState|caseIntact(1/0)|modalities.join(",")|selfCoherence`), so a manifest
signed on-device verifies under `verifyManifest()`. Signing is **fail-closed**: no key ⇒ no manifest,
never an unsigned/forgeable one.

## Verified SKU: no measured boot ⇒ bootState is "unverified"

Because we never burn Secure Boot eFuses (re-flashability is a hard requirement), the standard verified
build cannot honestly claim measured boot — `manifest.cpp` reports `bootState = "unverified"`. Only a
build with a genuine measured-boot attestation (e.g. anchored through the secure element) should report
`"secure"`. Don't be tempted to hardcode `"secure"` to look stronger; the platform treats it as a
claim and an unverifiable one buys nothing.

## Verified SKU: tamper is EVIDENT, not PROOF

`src/tamper.cpp` latches an irreversible "case was opened" flag on the first broken-loop reading. On a
non-SE build that latch is an NVS flag (best-effort — an attacker who can reflash/wipe NVS can clear
it). On the `-DCAGI_SECURE_ELEMENT` build the authoritative latch is the SE's tamper input: tripping it
locks the protected key, so an opened unit can no longer sign at all, and clearing it needs a factory SE
re-attestation. The seed for the non-SE signer must be persisted at provisioning time (NVS key
`devseed`) — `TODO(provisioning)` in `manifest.cpp`; until `ble_prov.cpp` writes it, a non-SE verified
build has no signer and `Manifest::emit` no-ops.

## Verified SKU: sensor HALs are stubs with honest TODOs

`src/sensors/{lidar,thermal,emi}.{h,cpp}` define clean, part-agnostic driver interfaces (a depth frame,
a thermal frame, an EMI spectrum) but ship as documented stubs that report `present() == false` until a
real part is wired (each names the reference part + the exact driver calls to fill in). That is
deliberate: the manifest/coherence plumbing is complete and testable end-to-end, and a bring-up just
fills a `read()` per populated sensor without touching the wiring above it.
