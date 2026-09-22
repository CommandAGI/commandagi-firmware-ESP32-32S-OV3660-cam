#pragma once
// Compile-time configuration for the CommandAGI ESP32-CAM firmware.

// Firmware version reported over BLE (INFO.fw) — bump on each release.
#define CAGI_FW_VERSION "1.2.0"

// Human model string reported over BLE (INFO.model).
#ifndef CAGI_MODEL
#define CAGI_MODEL "AI-Thinker ESP32-CAM"
#endif

// ── BLE provisioning GATT contract ──────────────────────────────────────────────────────────────
// MUST stay in sync with packages/domain/core/src/esp32cam.ts (the shared source of truth used by the apps).
#define CAGI_CAM_SERVICE_UUID   "c0a1d61c-0001-4a17-9c0a-1d61cab00001"
#define CAGI_CAM_CHAR_INFO      "c0a1d61c-0002-4a17-9c0a-1d61cab00001"  // read       — Esp32CamInfo JSON
#define CAGI_CAM_CHAR_STATUS    "c0a1d61c-0003-4a17-9c0a-1d61cab00001"  // read+notify — Esp32CamStatus JSON
#define CAGI_CAM_CHAR_PROVISION "c0a1d61c-0004-4a17-9c0a-1d61cab00001"  // write      — Esp32CamProvisioning JSON
#define CAGI_CAM_CHAR_COMMAND   "c0a1d61c-0005-4a17-9c0a-1d61cab00001"  // write      — identify|reboot|factory-reset
#define CAGI_CAM_MTU            512

// BLE advertised name prefix (a 4-hex MAC suffix is appended, e.g. "CommandAGI Cam A1B2").
#define CAGI_ADV_NAME_PREFIX "CommandAGI Cam "

// ── Provisioning security (PIN-keyed AEAD) ──────────────────────────────────────────────────────
// When CAGI_PROV_SECURE is 1 (default), the app must PIN-seal the PROVISION payload (HKDF-SHA256 →
// AES-256-GCM, keyed by CAGI_PROV_PIN + a per-boot salt) — a BLE sniffer can't read the Wi-Fi
// password or account key without the PIN. The PIN is an OUT-OF-BAND secret: print a unique one on
// each unit's label. The default below is for the reference build only — CHANGE IT per device.
#define CAGI_PROV_SECURE 1
#ifndef CAGI_PROV_PIN
#define CAGI_PROV_PIN "123456"
#endif
// HKDF `info` — keep in lockstep with CAGI_PROV_HKDF_INFO in packages/domain/core/src/esp32cam.ts.
#define CAGI_PROV_HKDF_INFO "cagi-cam-prov-v1"

// Optional defense-in-depth: also require BLE link-layer bonding + a passkey (the OS prompts for it).
// Off by default because OS passkey dialogs are inconsistent across platforms (BlueZ/Web Bluetooth),
// and the app-layer AEAD above already protects the secrets. Set to 1 to enforce hardware bonding.
#define CAGI_BLE_REQUIRE_BONDING 0
#define CAGI_BLE_PASSKEY 123456

// ── Streaming ───────────────────────────────────────────────────────────────────────────────────
// Target ms between frames. Frames now stream over a persistent WebSocket (cloud_ws), so the cost per
// frame is just the capture + a binary send — no per-frame HTTP. Default 33ms (~30fps); the real rate
// is bounded by capture + Wi-Fi. The server can push a slower cadence (battery/bandwidth); the floor is
// CAGI_FRAME_INTERVAL_MIN_MS.
#define CAGI_FRAME_INTERVAL_MS     33
#define CAGI_FRAME_INTERVAL_MIN_MS 33
#define CAGI_STREAM_CHANNEL    "cam"

// The classic ESP32 shares ONE radio between Wi-Fi and BLE; while BLE is active the coexistence layer
// FORCES Wi-Fi modem-sleep, which throttles every TLS upload to seconds on a weak link. So once the
// camera is actually streaming we STOP BLE and disable modem-sleep for full Wi-Fi throughput. If the
// stream then stays down this long (network/account problem), we re-advertise so the app can re-pair.
#define CAGI_BLE_REVIVE_MS     45000

// How often (ms) to ask the API for the desired sensor state (cam/mic on/off) when the device is
// NOT actively streaming. While streaming, the desired state piggybacks on each frame/audio POST
// response (zero extra requests), so this only paces the idle "is a sensor turned back on?" poll.
#define CAGI_CONTROL_POLL_MS   3000

// ── I2S microphone (INMP441) ──────────────────────────────────────────────────────────────────────
// Optional omnidirectional MEMS mic on I2S. **OFF by default** and opt-in at build time, because of a
// hardware constraint on the classic ESP32: the camera driver owns the I2S0 peripheral for its pixel
// DMA, and bringing up the legacy I2S driver (even on I2S1, even just to *probe* for a mic) disturbs
// that shared I2S/DMA state and stops the camera returning frames — a black "starting camera" screen.
// Proven on real hardware. So we cannot safely auto-detect a mic at runtime: the detection itself
// kills the camera. Camera-only boards (the common case) therefore build with audio OFF and always
// stream. A board that actually has an INMP441 wired opts in at build time:
//
//     pio run -e esp32cam -t upload                       # camera only (default) — always works
//     PLATFORMIO_BUILD_FLAGS="-DCAGI_AUDIO_ENABLED=1" \
//       pio run -e esp32cam -t upload                     # camera + mic (mic-equipped boards)
//
// (Equivalently `make firmware-flash MIC=1`.) When enabled, the mic runs on I2S1 and the firmware
// still presence-probes so a misconfigured build degrades gracefully.
#ifndef CAGI_AUDIO_ENABLED
#define CAGI_AUDIO_ENABLED 0
#endif
#define CAGI_AUDIO_CHANNEL    "mic"
#define CAGI_AUDIO_SAMPLE_RATE 16000   // 16 kHz mono PCM16 — speech-grade, small clips
#define CAGI_AUDIO_CLIP_MS    1000     // length of each posted clip (ms)
// Gain: INMP441 samples arrive left-justified in a 32-bit slot (24 valid bits). We take the top 16
// bits for PCM16; this right-shift sets loudness. 11 ≈ a sane default for room-level speech.
#define CAGI_AUDIO_SHIFT      11
#if defined(CAM_BOARD_ESP32S3)
  #define CAGI_I2S_SCK_PIN    41   // BCLK
  #define CAGI_I2S_WS_PIN     42   // LRCL / word-select
  #define CAGI_I2S_SD_PIN     2    // DOUT (mic → ESP)
#else
  // AI-Thinker ESP32-CAM: GPIO14/15/13 are the HS2 SD-card pins, free when no card is used.
  #define CAGI_I2S_SCK_PIN    14   // BCLK
  #define CAGI_I2S_WS_PIN     15   // LRCL / word-select
  #define CAGI_I2S_SD_PIN     13   // DOUT (mic → ESP)
#endif

// ── Verified-camera SKU (multi-modal + tamper-evident) ──────────────────────────────────────────
// The verified SKU is a SEPARATE board build: an ESP32 plus a secure element (ATECC608-class), a
// chassis tamper switch (or conductive mesh), and one or more corroborating sensors (LiDAR / thermal /
// EMI). It signs a per-window {@link CaptureManifest} (packages/domain/core integrity.ts) describing its
// firmware, boot state, case-intact reading, and active modalities, and streams it as a sidecar
// alongside frames. The platform RE-VERIFIES everything and recomputes cross-modal coherence from the
// actual frames — the device never self-certifies its own integrity strength.
//
// ALL of this is behind CAGI_VERIFIED_SKU (OFF by default) so the stock AI-Thinker camera build is
// byte-for-byte unchanged. Per-sensor flags gate each HAL driver independently — a unit with LiDAR but
// no thermal sets only CAGI_SENSOR_LIDAR. Enable at build time, e.g.:
//
//     PLATFORMIO_BUILD_FLAGS="-DCAGI_VERIFIED_SKU=1 -DCAGI_SENSOR_LIDAR=1 -DCAGI_SENSOR_THERMAL=1 \
//       -DCAGI_SENSOR_EMI=1 -DCAGI_SECURE_ELEMENT=1" pio run -e esp32cam-verified -t upload
//
// HARD CONSTRAINT (docs/platform/CAMERAS.md): the verified SKU stays USB-re-flashable forever. We do
// NOT burn Secure Boot / Flash Encryption eFuses — the signing key's confidentiality comes from the
// secure element (it never leaves the SE), NOT from locking the board. Re-flashability is preserved.
#ifndef CAGI_VERIFIED_SKU
#define CAGI_VERIFIED_SKU 0
#endif

// The signing key lives in a discrete secure element over I2C instead of NVS. When 1, manifest.cpp asks
// the SE to sign (the private key never enters ESP32 RAM). When 0 on a verified build, the manifest is
// signed with the NVS-stored device seed (the `device_key` tier).
//
// CRITICAL PART CONSTRAINT (hardware/VERIFIED_SKU.md §3): the platform verifies **Ed25519** (integrity.ts
// verifyFrameChain/verifyManifest use @noble/ed25519). The reflexive ATECC608 does **ECDSA P-256 ONLY —
// no Ed25519** — so it CANNOT sign what the platform accepts. The recommended SE is therefore an
// Ed25519-capable part: **NXP SE050** (EdDSA/Ed25519 native, attested keygen), or Microchip TA100 /
// Infineon OPTIGA Trust M (verify the rev supports Ed25519). Using a P-256-only SE would force a second
// platform verification path + a per-device sigAlg field (a real fragmentation cost) — see §3.1.
#ifndef CAGI_SECURE_ELEMENT
#define CAGI_SECURE_ELEMENT 0
#endif
// SE I2C address (7-bit). Default is the NXP SE050 (0x48). (ATECC608 would be 0x60 — but it is P-256
// only and not recommended; see the constraint above.) Reconcile against the actual schematic at bring-up.
#ifndef CAGI_SE_I2C_ADDR
#define CAGI_SE_I2C_ADDR 0x48
#endif
// SE bus + enable. The SE sits on its own I2C-B in the protected/tamper zone (VERIFIED_SKU.md §7). On the
// S3 verified build, buried I2C-B on the UART0 pins (43/44) keeps SE traffic off the shared sensor bus;
// a board may instead share the sensor I2C-A — set these to CAGI_VERIFIED_I2C_SDA/SCL. ENA is tied high
// or gated by a GPIO. Placeholders reconciled at schematic bring-up.
#ifndef CAGI_SE_SDA_GPIO
#define CAGI_SE_SDA_GPIO 43
#endif
#ifndef CAGI_SE_SCL_GPIO
#define CAGI_SE_SCL_GPIO 44
#endif
#ifndef CAGI_SE_ENA_GPIO
#define CAGI_SE_ENA_GPIO -1   // -1 = ENA tied high (always enabled); set a GPIO to gate the SE
#endif

// ── Chassis tamper switch ────────────────────────────────────────────────────────────────────────
// A normally-closed loop across the enclosure seam, read on a GPIO with an internal pull-up: closed
// (case shut) reads LOW; opening the case breaks the loop and the pin floats HIGH. The first HIGH
// reading LATCHES an irreversible "case was opened" flag in NVS (tamper-EVIDENT, not tamper-proof).
#ifndef CAGI_TAMPER_ENABLED
#define CAGI_TAMPER_ENABLED CAGI_VERIFIED_SKU
#endif
// NC loop → GND; INPUT_PULLUP. NOTE: on the ESP32-S3 verified board GPIO12 is a CAMERA DATA line
// (Y7_GPIO_NUM), so the tamper loop MUST move to a free pin there — GPIO7. The AI-Thinker default keeps
// 12 (free HS2 pin on that board). See VERIFIED_SKU.md §4.5/§5.
#ifndef CAGI_TAMPER_GPIO
  #if defined(CAM_BOARD_ESP32S3)
    #define CAGI_TAMPER_GPIO 7
  #else
    #define CAGI_TAMPER_GPIO 12
  #endif
#endif

// ── Corroborating sensors (each independently gated) ────────────────────────────────────────────
// Solid-state LiDAR / ToF depth (e.g. VL53L5CX zone ToF, or a scanning module over I2C/UART). A real
// 3D scene has depth variance; a flat display reads as a plane → high flatness → the platform's
// coherence check drops. The driver contributes a coarse depth frame the manifest declares as "lidar".
#ifndef CAGI_SENSOR_LIDAR
#define CAGI_SENSOR_LIDAR 0
#endif
// Thermal / long-wave IR array (e.g. MLX90640 32x24). A real scene has a temperature distribution; a
// display panel is a near-uniform temperature. Contributes a low-res thermal frame declared as "ir".
#ifndef CAGI_SENSOR_THERMAL
#define CAGI_SENSOR_THERMAL 0
#endif
// EMI / RF near-field probe (a short antenna into an ADC / SDR front-end). A nearby display emits a
// characteristic refresh-rate EM signature; its presence is a replay tell. Contributes a coarse EMI
// spectrum declared as "emi".
#ifndef CAGI_SENSOR_EMI
#define CAGI_SENSOR_EMI 0
#endif
// ── Verified-SKU sensor bus + pin map (S3 reference board; see VERIFIED_SKU.md §4.5) ────────────────
// The corroborating sensors (LiDAR + thermal) share ONE I2C-A bus at distinct addresses; the SE has its
// own I2C-B (above). These are the free ESP32-S3 GPIOs on the [env:esp32cam-verified] board — reconcile
// against the real schematic at bring-up. On non-S3 boards, override to that board's free pins.
#ifndef CAGI_VERIFIED_I2C_SDA
#define CAGI_VERIFIED_I2C_SDA 8   // shared sensor I2C data (VL53L5CX 0x29 + MLX90640 0x33)
#endif
#ifndef CAGI_VERIFIED_I2C_SCL
#define CAGI_VERIFIED_I2C_SCL 9   // shared sensor I2C clock (up to 1 MHz for the MLX90640)
#endif
// LiDAR (VL53L5CX) control lines: LPn = shutdown/address-select, INT = data-ready (poll fallback if -1).
#ifndef CAGI_LIDAR_LPN_GPIO
#define CAGI_LIDAR_LPN_GPIO 5
#endif
#ifndef CAGI_LIDAR_INT_GPIO
#define CAGI_LIDAR_INT_GPIO 6
#endif
// EMI front-end ADC inputs (ADC1 only — ADC2 is unusable while Wi-Fi is up). CH0 = envelope-detected
// near-field E-field probe; the flicker channel = photodiode TIA. Both FFT'd on-device (arduinoFFT).
#ifndef CAGI_EMI_ADC_PIN
#define CAGI_EMI_ADC_PIN 1        // ADC1_CH0 — near-field EM refresh/pixel-clock signature
#endif
#ifndef CAGI_EMI_FLICKER_ADC_PIN
#define CAGI_EMI_FLICKER_ADC_PIN 4  // ADC1_CH3 — optical flicker (display PWM/refresh), EM-shield-proof
#endif

// The stream sidecar channel the signed manifest is announced on (parallel to CAGI_STREAM_CHANNEL).
#define CAGI_MANIFEST_CHANNEL "manifest"
// How often (ms) to emit a fresh signed manifest while streaming. The manifest describes a capture
// WINDOW, not each frame; ~2s keeps the case-intact/boot/modality claims current without flooding the
// socket. Its `seq` binds it to the frame chain segment it covers.
#define CAGI_MANIFEST_INTERVAL_MS 2000

// ── NVS (persistent creds) ──────────────────────────────────────────────────────────────────────
#define CAGI_NVS_NAMESPACE "cagi"  // a factory-reset erases exactly this namespace
// Tamper latch key inside the namespace. NOTE: a factory-reset erases the whole `cagi` namespace and so
// would clear this latch too — on the CAGI_SECURE_ELEMENT variant the AUTHORITATIVE latch lives in the
// secure element's tamper register (see tamper.cpp), which a factory-reset cannot clear; the NVS copy
// is only a fast-read mirror. On a non-SE build the NVS latch is best-effort (tamper-evident) only.
#define CAGI_TAMPER_NVS_KEY "tamper_open"

// ── Camera pin map ──────────────────────────────────────────────────────────────────────────────
// AI-Thinker ESP32-CAM is the default. For an ESP32-S3 board define CAM_BOARD_ESP32S3 (in
// platformio.ini build_flags) and fill in its pins below.
#if defined(CAM_BOARD_DFR_S3_AICAM)
  // DFRobot ESP32-S3 AI Camera (SKU DFR1154) — OV3660, 16MB flash + 8MB PSRAM. Pin map verified
  // against DFRobot/DFR1154_Examples (CameraWebServer). On-board LED is GPIO3; an IR illuminator
  // sits on GPIO47. Build env: [env:esp32cam-s3] (-DCAM_BOARD_DFR_S3_AICAM).
  #define PWDN_GPIO_NUM   -1
  #define RESET_GPIO_NUM  -1
  #define XCLK_GPIO_NUM    5
  #define SIOD_GPIO_NUM    8
  #define SIOC_GPIO_NUM    9
  #define Y9_GPIO_NUM      4
  #define Y8_GPIO_NUM      6
  #define Y7_GPIO_NUM      7
  #define Y6_GPIO_NUM     14
  #define Y5_GPIO_NUM     17
  #define Y4_GPIO_NUM     21
  #define Y3_GPIO_NUM     18
  #define Y2_GPIO_NUM     16
  #define VSYNC_GPIO_NUM   1
  #define HREF_GPIO_NUM    2
  #define PCLK_GPIO_NUM   15
  #define LED_GPIO_NUM     3  // on-board LED — used as the "identify" indicator
#elif defined(CAM_BOARD_ESP32S3)
  // Seeed XIAO ESP32-S3 Sense (example) — adjust to your board.
  #define PWDN_GPIO_NUM   -1
  #define RESET_GPIO_NUM  -1
  #define XCLK_GPIO_NUM   10
  #define SIOD_GPIO_NUM   40
  #define SIOC_GPIO_NUM   39
  #define Y9_GPIO_NUM     48
  #define Y8_GPIO_NUM     11
  #define Y7_GPIO_NUM     12
  #define Y6_GPIO_NUM     14
  #define Y5_GPIO_NUM     16
  #define Y4_GPIO_NUM     18
  #define Y3_GPIO_NUM     17
  #define Y2_GPIO_NUM     15
  #define VSYNC_GPIO_NUM  38
  #define HREF_GPIO_NUM   47
  #define PCLK_GPIO_NUM   13
  #define LED_GPIO_NUM    21
#else
  // AI-Thinker ESP32-CAM (default).
  #define PWDN_GPIO_NUM   32
  #define RESET_GPIO_NUM  -1
  #define XCLK_GPIO_NUM    0
  #define SIOD_GPIO_NUM   26
  #define SIOC_GPIO_NUM   27
  #define Y9_GPIO_NUM     35
  #define Y8_GPIO_NUM     34
  #define Y7_GPIO_NUM     39
  #define Y6_GPIO_NUM     36
  #define Y5_GPIO_NUM     21
  #define Y4_GPIO_NUM     19
  #define Y3_GPIO_NUM     18
  #define Y2_GPIO_NUM      5
  #define VSYNC_GPIO_NUM  25
  #define HREF_GPIO_NUM   23
  #define PCLK_GPIO_NUM   22
  #define LED_GPIO_NUM     4  // on-board flash LED — used as an "identify" indicator
#endif

// XCLK frequency. 16 MHz is empirically the sweet spot for the OV2640 clones on AI-Thinker boards
// (higher, stable fps). The OV3660 on the DFRobot ESP32-S3 AI Camera needs the sensor's standard
// 20 MHz — at 16 MHz it inits over SCCB but never emits a frame ("sensor returned no frame").
#ifndef CAGI_XCLK_HZ
  #if defined(CAM_BOARD_DFR_S3_AICAM)
    #define CAGI_XCLK_HZ 20000000
  #else
    #define CAGI_XCLK_HZ 16000000
  #endif
#endif
