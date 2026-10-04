#pragma once
// Compile-time configuration for the CommandAGI ESP32-CAM firmware.
#include <sdkconfig.h>  // CONFIG_IDF_TARGET_* (the SoC decides the audio paths below)

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
//     PLATFORMIO_BUILD_FLAGS="-DCAGI_AUDIO_ENABLED=1" pio run -e esp32cam -t upload   # camera + mic
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
#if defined(CAM_BOARD_DFR_S3_AICAM)
  // DFR1154: an on-board MSM261DGT003 PDM mic, not an INMP441. Pins from DFRobot's schematic
  // (DFR1154 Schematic v1.1, nets PDM_CLK = GPIO38, PDM_DATA = GPIO39; mic L/R tied to GND by R3) and
  // DFRobot/DFR1154_Examples "5.2 Recording & Playback" (setPinsPdmRx(GPIO_NUM_38, GPIO_NUM_39)).
  // The S3 has PDM RX on I2S0 only. The S3 camera driver uses LCD_CAM, not I2S (libesp32-camera.a for
  // esp32s3 links LCD_CAM and no I2S symbol), so I2S0 is free on this board.
  #define CAGI_MIC_PDM        1
  #define CAGI_PDM_CLK_PIN    38
  #define CAGI_PDM_DATA_PIN   39
#elif defined(CAM_BOARD_ESP32S3)
  #define CAGI_I2S_SCK_PIN    41   // BCLK
  #define CAGI_I2S_WS_PIN     42   // LRCL / word-select
  #define CAGI_I2S_SD_PIN     2    // DOUT (mic → ESP)
#else
  // AI-Thinker ESP32-CAM: GPIO14/15/13 are the HS2 SD-card pins, free when no card is used.
  #define CAGI_I2S_SCK_PIN    14   // BCLK
  #define CAGI_I2S_WS_PIN     15   // LRCL / word-select
  #define CAGI_I2S_SD_PIN     13   // DOUT (mic → ESP)
#endif

#ifndef CAGI_MIC_PDM
#define CAGI_MIC_PDM 0
#endif
// On the ESP32-S3 the mic runs in its own FreeRTOS task into a PSRAM double buffer, so a clip never
// blocks the frame loop. The classic ESP32 keeps the blocking capture: its camera owns I2S0 and that
// path is proven on hardware, so we do not change it.
#if CONFIG_IDF_TARGET_ESP32S3
#define CAGI_AUDIO_ASYNC 1
#else
#define CAGI_AUDIO_ASYNC 0
#endif

// ── Speaker (I2S amplifier, e.g. the DFR1154's MAX98357A) ───────────────────────────────────────
// OFF by default, so other builds do not change. A build with -DCAGI_SPEAKER_ENABLED=1 declares a
// `speaker` output and the `play_audio` / `present_stop` controls on the realtime socket
// (cloud_ws.cpp; the wire contract is in README.md § Speaker). The platform checks the standing grant
// and records the command before it sends it; the device reports what it did in `action_result`.
// That report is the device's claim. It is not proof that a sound came out of the speaker.
#ifndef CAGI_SPEAKER_ENABLED
#define CAGI_SPEAKER_ENABLED 0
#endif
#if CAGI_SPEAKER_ENABLED
  #if !CONFIG_IDF_TARGET_ESP32S3
    #error "CAGI_SPEAKER_ENABLED needs an ESP32-S3: on the classic ESP32 the camera owns I2S0 and the mic I2S1"
  #endif
  // Fixed maximum digital gain (0 < gain <= 1.0) applied to every sample before the amplifier. No
  // command can change it: the wire contract has no level field. 0.5 = -6 dBFS peak.
  #ifndef CAGI_SPEAKER_MAX_GAIN
  #define CAGI_SPEAKER_MAX_GAIN 0.5
  #endif
  #define CAGI_SPEAKER_MAX_BYTES  (1024u * 1024u)  // a clip larger than 1 MB is refused
  #define CAGI_SPEAKER_MAX_MS     30000u            // a clip longer than 30 s is refused
  // The main loop does not post mic clips that overlap playback (plus this tail), so the platform does
  // not hear the device's own voice.
  #define CAGI_SPEAKER_DUCK_TAIL_MS 300
  #if defined(CAM_BOARD_DFR_S3_AICAM)
    // MAX98357A (U10) nets from DFR1154 Schematic v1.1: BCLK = GPIO45, LRCLK = GPIO46, DIN = GPIO42,
    // SD_MODE# = GPIO40 (R1 100k pull-up to 3V3), GAIN_SLOT via R6 100k = GPIO41. BCLK/LRCLK/DIN match
    // DFRobot/DFR1154_Examples (i2s1.setPins(45, 46, 42); "6.10 PlayOnlineMusic" I2S_BCLK 45, I2S_LRC 46,
    // I2S_DOUT 42). The firmware drives SD_MODE# low (amplifier shut down) except while a clip plays.
    // It leaves GAIN high-impedance: GAIN_SLOT then floats behind R6, which the MAX98357A datasheet
    // gives as 9 dB (the state DFRobot's examples use).
    #define CAGI_SPK_BCLK_PIN   45
    #define CAGI_SPK_LRCLK_PIN  46
    #define CAGI_SPK_DIN_PIN    42
    #define CAGI_SPK_SD_PIN     40
    #define CAGI_SPK_GAIN_PIN   41
  #endif
  #if !defined(CAGI_SPK_BCLK_PIN) || !defined(CAGI_SPK_LRCLK_PIN) || !defined(CAGI_SPK_DIN_PIN)
    #error "CAGI_SPEAKER_ENABLED: define CAGI_SPK_BCLK_PIN, CAGI_SPK_LRCLK_PIN and CAGI_SPK_DIN_PIN for this board"
  #endif
  #ifndef CAGI_SPK_SD_PIN
  #define CAGI_SPK_SD_PIN -1
  #endif
  #ifndef CAGI_SPK_GAIN_PIN
  #define CAGI_SPK_GAIN_PIN -1
  #endif
#endif

// ── Cellular (LTE Cat-1 modem over UART, PPP) ───────────────────────────────────────────────────
// OFF by default. A build with -DCAGI_CELLULAR_ENABLED=1 drives a SIMCom A7670-class modem: PWRKEY
// power-on, AT setup (SIM PIN, APN, LTE only), then PPP into lwIP, so HTTPS and the realtime socket
// run over the modem unchanged. It needs lwIP with PPP, which arduino-esp32 2.x's precompiled libs
// leave out, so cellular envs build Arduino as an ESP-IDF component (`framework = arduino, espidf`,
// sdkconfig.defaults.<soc> = Arduino's own sdkconfig + CONFIG_LWIP_PPP_SUPPORT). README § Cellular.
// Link policy: Wi-Fi when it is provisioned and connected, else cellular.
#ifndef CAGI_CELLULAR_ENABLED
#define CAGI_CELLULAR_ENABLED 0
#endif
#if CAGI_CELLULAR_ENABLED
  #if !CONFIG_LWIP_PPP_SUPPORT
    #error "CAGI_CELLULAR_ENABLED needs lwIP PPP: build with framework = arduino, espidf and CONFIG_LWIP_PPP_SUPPORT=y"
  #endif
  // Frame interval floor on cellular (ms). At QVGA, JPEG quality 12, a frame is ~10-20 kB, so the
  // default 33 ms is ~1.1-2.2 GB per hour; 1000 ms is ~36-72 MB per hour. The server can set its own
  // floor with `cellularIntervalMs` in a control response.
  #ifndef CAGI_CELLULAR_INTERVAL_MS
  #define CAGI_CELLULAR_INTERVAL_MS 1000
  #endif
  #define CAGI_MODEM_BAUD       115200  // the A7670's power-on rate
  #ifndef CAGI_MODEM_BAUD_FAST
  #define CAGI_MODEM_BAUD_FAST  921600  // AT+IPR after sync; the driver falls back to 115200 if it fails
  #endif
  // Pins from the product READMEs (hardware/CommandAGI-Cam-00x/README.md § modem pin map). PWRKEY and
  // RESET drive a transistor that pulls the modem pin low: HIGH = pressed / held. -1 = not wired.
  #if defined(CAM_BOARD_DFR_S3_AICAM)
    // Cam-002 carrier: Gravity connector (GPIO43/44) + the microSD contacts (GPIO11/12/13).
    #define CAGI_MODEM_UART        1
    #define CAGI_MODEM_TX_PIN     43   // → modem RXD
    #define CAGI_MODEM_RX_PIN     44   // ← modem TXD (internal pull-up: the shifter floats while the modem is off)
    #define CAGI_MODEM_PWRKEY_PIN 11
    #define CAGI_MODEM_STATUS_PIN 12   // HIGH = modem on (internal pull-down)
    #define CAGI_MODEM_RESET_PIN  13
    #define CAGI_MODEM_RAIL_OFF_PIN -1
  #elif defined(CAM_BOARD_AITHINKER)
    // Cam-001 base board: the five free header pins. IO13/14/15 are also the INMP441 pins.
    #if CAGI_AUDIO_ENABLED
      #error "On the AI-Thinker board the modem uses IO13/14/15, the INMP441's pins: enable one of them"
    #endif
    #define CAGI_MODEM_UART        2
    #define CAGI_MODEM_TX_PIN     14   // → modem RXD
    #define CAGI_MODEM_RX_PIN     13   // ← modem TXD
    // IO12 (flash voltage) and IO2 (boot mode) are strap pins. The firmware drives them HIGH only in
    // short pulses (PWRKEY 100 ms; rail off 3 s, recovery only) and LOW at all other times. Any reset
    // makes them inputs, so the base board's 10 kΩ pull-downs hold both LOW when the straps are read.
    #define CAGI_MODEM_PWRKEY_PIN 12
    #define CAGI_MODEM_STATUS_PIN 15
    #define CAGI_MODEM_RESET_PIN  -1
    #define CAGI_MODEM_RAIL_OFF_PIN 2  // HIGH = modem supply off; recovery only
  #endif
  #ifndef CAGI_MODEM_UART
    #error "CAGI_CELLULAR_ENABLED: define the CAGI_MODEM_* pins for this board"
  #endif
#endif

// ── Battery (optional ADC divider + charger STAT) ───────────────────────────────────────────────
// A board with -DCAGI_BATTERY_ADC_PIN=<gpio> reports the cell voltage and an estimated percent in BLE
// STATUS and in the runtime `status` message. CAGI_BATTERY_DIVIDER is Vcell / Vpin (2.0 for the
// Cam-002 battery variant's 100k/100k). A charger STAT pin (-DCAGI_BATTERY_STAT_PIN) adds
// `charging`; CAGI_BATTERY_STAT_ACTIVE is its level while it charges (open-drain STAT: LOW).
#ifndef CAGI_BATTERY_ADC_PIN
#define CAGI_BATTERY_ADC_PIN -1
#endif
#ifndef CAGI_BATTERY_DIVIDER
#define CAGI_BATTERY_DIVIDER 2.0
#endif
#ifndef CAGI_BATTERY_STAT_PIN
#define CAGI_BATTERY_STAT_PIN -1
#endif
#ifndef CAGI_BATTERY_STAT_ACTIVE
#define CAGI_BATTERY_STAT_ACTIVE LOW
#endif

// How often (ms) the device sends a runtime `status` message with its link and battery while the
// realtime socket is open (also on connect and on a link change). Cellular and battery builds only.
#define CAGI_RUNTIME_STATUS_MS 60000

// ── Sealed stream (the device seals its own frames) ────────────────────────────────────────────
// OFF by default. A build with -DCAGI_DEVICE_SEALS=1 makes an Ed25519 key on its first boot (NVS
// namespace `cagi-seal`), stamps each frame with its capture time, indexes it in the `cam-at` channel
// and seals each stream once a second with deployments/clients/seal-c (src/seal/). Both
// channels are declared `seals: "device"`, so the recorder keeps them byte for byte. Frames then go as
// addressed `frame` messages, not bare binary. README § Sealed stream; docs/integrity.md in CommandAGI.
#ifndef CAGI_DEVICE_SEALS
#define CAGI_DEVICE_SEALS 0
#endif
#define CAGI_SEAL_INDEX_CHANNEL "cam-at"

// ── NVS (persistent creds) ──────────────────────────────────────────────────────────────────────
#define CAGI_NVS_NAMESPACE "cagi"  // a factory-reset erases exactly this namespace (never `cagi-seal`, the sealing key)

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
