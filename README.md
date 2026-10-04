# ESP32-32S-OV3660-cam

Part of [CommandAGI](https://commandagi.com): connecting agents to real computers, robots and
physical environments. This repository can be cloned independently of the private platform code.

```sh
git clone https://github.com/CommandAGI/commandagi-firmware-ESP32-32S-OV3660-cam.git
cd commandagi-firmware-ESP32-32S-OV3660-cam
```

## Hardware status

The repository name identifies the requested hardware target. The checked-in PlatformIO default
currently uses the AI-Thinker `esp32cam` board with OV2640 wiring; an ESP32-32S/OV3660 build has not
been verified in this cleanup. Review `platformio.ini` and `src/config.h` against your exact board
before flashing. The verified-camera design documents describe additional hardware, not a claim
that those peripherals are implemented or certified.


Turn a ~$6 **AI-Thinker ESP32-CAM** into a camera that streams into your CommandAGI dashboard. You
pair it from the CommandAGI **mobile app** over Bluetooth — no hardcoded Wi-Fi, no re-flashing to
change networks, no cloud account baked into the binary.

```
power on ──► advertises over BLE ──► app finds it ("My cameras → Pair")
        ──► app sends Wi-Fi creds + a per-device account key over BLE
        ──► camera joins Wi-Fi, registers a machine session under your account
        ──► streams JPEG frames ──► shows up live in the dashboard
```

It remembers everything in flash (NVS), so after a power cycle it reconnects and resumes streaming on
its own. See [the hardware notes](hardware/README.md) for the board and security design.

## Hardware

- **AI-Thinker ESP32-CAM** (ESP32 + OV2640, 4 MB flash, PSRAM) — the default.
- A USB-UART adapter (FTDI/CP2102) for the first flash, or an ESP32-CAM-MB programmer board.
- Other boards (e.g. ESP32-S3 cams) work by editing the pin map in [`src/config.h`](src/config.h) and
  adding `-DCAM_BOARD_ESP32S3` to `build_flags`.
- **Verified-camera SKU** (a higher-tier board): ESP32-S3 + a secure element holding the signing key +
  a chassis tamper switch + corroborating sensors (LiDAR / thermal-IR / EMI). It signs a per-window
  capture manifest and streams it as a sidecar so the platform can prove the pixels came from a genuine,
  unopened, multi-modal device — much harder to fool with a screen/print replay. See
  [`hardware/README.md`](hardware/README.md) for the BOM + the "tamper-evident, not tamper-proof"
  caveat, and build it with `pio run -e esp32cam-verified -t upload`. It stays **re-flashable** (no
  eFuse burns — the key's confidentiality comes from the secure element, not from locking the board).

## Flashing

Uses [PlatformIO](https://platformio.org/). The AI-Thinker board has no auto-reset, so for the
**first** flash tie **GPIO0 → GND**, power-cycle, then upload; remove the jumper and reset to run.

```bash
cd .
pio run                 # build
pio run -t upload       # flash over USB-UART
pio device monitor      # watch the serial log (115200 baud)
```

> **Re-flashable forever.** This firmware deliberately does **not** enable Secure Boot or Flash
> Encryption, and never burns the download-disable eFuse. The partition table keeps two OTA app slots
> ([`partitions.csv`](partitions.csv)) so you can always re-flash over USB **and** push OTA updates
> later. A factory-reset wipes only the stored credentials (the `cagi` NVS namespace), never the app.

### Why it can ALWAYS be re-flashed (the guarantee)

The ESP32's serial **download mode lives in mask ROM** and runs _before_ the application on every
reset where GPIO0 is held low — so no firmware can ever block re-flashing. The only thing that can
permanently lock a board is **burning an eFuse** (Secure Boot, Flash Encryption, or the
download-disable bit), and this firmware does **none** of that:

- No `esp_efuse_*` / `esp_secure_boot_*` / `esp_flash_encrypt_*` calls anywhere in `src/`.
- No Secure Boot / Flash Encryption build flags — `platformio.ini` produces a plain, unencrypted image.
- The partition table's `Flags` column is empty (no `encrypted` partitions).
- `factory-reset` calls `Preferences.clear()` on just the `cagi` NVS namespace — never `nvs_flash_erase()`
  or a partition wipe, and never the app.

**Recovery (even a "bricked" board):** tie **GPIO0 → GND**, press reset (or power-cycle), and run
`pio run -t upload` (or `esptool.py write_flash`). Because download mode is ROM-based, this works no
matter what state the app is in — a boot loop, a bad OTA, or garbage in flash. Remove the jumper and
reset to run again. `esptool.py erase_flash` then a fresh `pio run -t upload` is the full clean slate.

## How pairing works (BLE GATT)

The device advertises service `c0a1d61c-0001-…` as **"CommandAGI Cam XXXX"** (XXXX = MAC suffix). The
contract is shared with the apps in ``packages/domain/core/src/esp32cam.ts`` (platform-internal reference)
— keep the UUIDs in [`src/config.h`](src/config.h) in sync with it.

| Characteristic | UUID suffix | Props         | Payload                                                                      |
| -------------- | ----------- | ------------- | ---------------------------------------------------------------------------- |
| INFO           | `…0002`     | read          | `{ kind, model, fw, hwid, name, provisioned, mic, spk?, secure, salt? }`     |
| STATUS         | `…0003`     | read + notify | `{ state, ip?, sessionId?, deviceId?, error? }`                              |
| PROVISION      | `…0004`     | write         | PIN-sealed `{ ssid, psk, apiBaseUrl, apiKey, deviceName? }` (see _Security_) |
| COMMAND        | `…0005`     | write         | `identify` \| `reboot` \| `factory-reset`                                    |

`state` walks `idle → wifi_connecting → registering → streaming` (with `wifi_failed` /
`register_failed` / `error` branches the app surfaces so you can retry). `paused` means online but
every sensor was turned off remotely (see _Remote sensor control_).

## Microphone (optional INMP441)

Wire an **INMP441** I2S MEMS mic and the camera also streams audio — it's fully optional and
independent: no mic ⇒ camera-only; no camera ⇒ mic-only; both ⇒ both. INFO advertises `mic: true`
when one is detected at boot.

| INMP441 | ESP32-CAM (AI-Thinker) | notes                                                  |
| ------- | ---------------------- | ------------------------------------------------------ |
| VDD     | 3V3                    |                                                        |
| GND     | GND                    |                                                        |
| L/R     | GND                    | selects the **left** channel (what the firmware reads) |
| WS      | GPIO15                 | word-select / LRCL (`CAGI_I2S_WS_PIN`)                 |
| SCK     | GPIO14                 | bit clock / BCLK (`CAGI_I2S_SCK_PIN`)                  |
| SD      | GPIO13                 | data out → ESP (`CAGI_I2S_SD_PIN`)                     |

GPIO13/14/15 are the unused HS2 SD-card pins; override them (and the ESP32-S3 defaults) in
[`src/config.h`](src/config.h). Audio is 16 kHz mono PCM16, posted as 1 s WAV clips to
`channel=mic&kind=audio`. Set `-DCAGI_AUDIO_ENABLED=0` to compile the mic out entirely.

### The DFR1154's on-board PDM mic

The DFRobot DFR1154 (`-DCAM_BOARD_DFR_S3_AICAM`) has an MSM261DGT003 PDM mic on the board. With
`-DCAGI_AUDIO_ENABLED=1` the firmware reads it in PDM RX mode on I2S0: clock on GPIO38, data on GPIO39
(DFR1154 Schematic v1.1, nets `PDM_CLK` / `PDM_DATA`; DFRobot's `DFR1154_Examples` 5.2 uses the same
pins). The S3 has PDM RX on I2S0 only. The S3 camera driver uses the LCD_CAM peripheral, not I2S, so
the mic and the camera do not share a peripheral. The mic's L/R pin is tied to GND; the firmware
probes the left slot first and then the right slot, and logs which one carries data.

### Capture without blocking video (ESP32-S3)

On an ESP32-S3 build a capture task records continuously into two 1 s WAV buffers in PSRAM. The
main loop posts a finished clip when one is ready and never waits for the mic. The task is pinned to
core 1 with the loop; the camera driver's task and Wi-Fi run on core 0. If the loop still posts the
previous clip when the next one is finished, the task drops the finished clip and logs it. The HTTPS
POST of a clip still runs in the loop and holds it for one round trip per second.

The classic ESP32 keeps the blocking capture (one clip per call, ~1 s). Its camera owns I2S0, and that
path is proven on hardware, so it does not change.

When the operator turns the mic off, the S3 capture task stops the I2S clock and drops a clip that
was not posted.

## Speaker (`-DCAGI_SPEAKER_ENABLED=1`, ESP32-S3 only)

A speaker build declares a `speaker` output on the realtime socket and plays audio clips that the
platform sends. Default is off (`0`), so other builds do not change. The DFR1154 pins come from its
schematic (Schematic v1.1, MAX98357A U10): BCLK GPIO45, LRCLK GPIO46, DIN GPIO42, SD_MODE# GPIO40,
GAIN_SLOT GPIO41 through 100 kΩ. The speaker uses I2S1 (the mic has I2S0).

The platform checks the standing grant and records the command before it sends it. The device does
not decide authority. INFO advertises `spk: true` when the speaker driver started.

### Wire contract (realtime socket `wss://…/rt/run/<sessionId>?device=…&role=agent&runtime=1`)

The same protocol as `packages/runtime/host-core/src/client.ts`.

1. After `{"type":"channels",…}` the device sends:

   ```json
   {"type":"outputs","outputs":[{"outputId":"speaker","kind":"speaker","label":"Speaker","primary":true}]}
   {"type":"controls","controls":[{"channelId":"ctrl","kind":"ctrl","label":"Presence",
     "actions":["play_audio","present_stop"],"payloadSchema":{"play_audio":{…},"present_stop":{…}}}]}
   ```

   The schemas are `PRESENCE_PAYLOAD_SCHEMAS` from `client.ts`. The device does not declare `say`: it
   has no text-to-speech. The platform makes a clip from `say` and sends `play_audio`.

2. The platform sends
   `{"type":"control","channelId":"ctrl","action":"play_audio","payload":{"url"?,"base64"?,"mime"?,"format"?,"outputId"?,"loop"?,"interrupt"?},"requestId"?}`.
   - `url`: `https://` only. The device sends its API key as `Authorization: Bearer` only when the URL's
     host is the API host it was provisioned with; the URL's own `token=` query does the rest.
   - `base64`: decoded on the device. **The socket library closes the socket (code 1009) on any
     message over 15 kB**, so a base64 clip must keep the whole message under 15 kB. Send a `url` for
     a larger clip.
   - Formats: MP3 (the platform's synthesized voice) and WAV PCM16, mono or stereo (mixed to mono),
     8–48 kHz. `format`, then `mime`, then the response's `Content-Type` (when it is `audio/*`) name the
     format; without one the device looks at the bytes. Anything else fails.
   - Limits: the device refuses a clip over 1 MB or over 30 s, and `loop: true`.
   - `interrupt` (default true) stops the current clip first. `interrupt: false` while a clip loads or
     plays fails with `busy: one clip at a time`.
   - `outputId` other than `speaker` fails.

3. The device replies once per `requestId`:
   - `{"type":"action_result","requestId":…,"action":"play_audio","result":{"ok":true,"durationMs":N}}`
     when playback starts. This is the device's report. It is not proof that sound came out.
   - `{"type":"action_result",…,"result":{"ok":false,"error":"…"}}` when it refuses or fails: a bad
     format, a limit, a failed fetch, `stopped`, `interrupted by a newer clip`, or
     `speaker output disabled by the operator`.
   - A result made while the socket is down is lost.

4. `present_stop` stops the clip that plays and drops one that waits; it replies `{ok:true}`.

5. `{"type":"config","outputs":{"speaker":false}}` disables the output: the device stops playback and
   refuses `play_audio` until a `config` with `outputs` that does not set `speaker: false`.

6. One clip at a time. The device never retries a clip; the platform or an agent decides to send it
   again, as a new command.

### Safety and level

- `CAGI_SPEAKER_MAX_GAIN` (default 0.5, that is −6 dBFS peak) multiplies every sample. It is a
  compile-time constant; no message can change it.
- The firmware holds the amplifier in shutdown (SD_MODE# low) except while a clip plays, and leaves
  GAIN_SLOT floating behind its 100 kΩ resistor (9 dB in the MAX98357A datasheet, the state DFRobot's
  examples use).

### The mic while the speaker plays

The mic keeps recording. The main loop does not post a clip recorded while the speaker played (plus
300 ms), so the platform does not hear the device's own voice. This is a duck, not echo cancellation.

## Remote sensor control

While streaming, each frame/audio POST response carries the operator's desired sensor state
(`{ sensors: { cam, mic } }`), so toggling a camera's cam/mic off in the dashboard stops that stream
within one interval. When every sensor is off the device goes `paused` and polls
`GET /v1/streams/:sessionId/:deviceId/control` every few seconds, so it can be turned back on
remotely (it never needs a power-cycle to resume).

### Security notes

- **The PROVISION payload is PIN-encrypted.** With `CAGI_PROV_SECURE` (default on), INFO advertises
  `secure: true` + a per-boot `salt`, and the app must seal the payload: `key = HKDF-SHA256(PIN, salt,
"cagi-cam-prov-v1")`, `wire = IV(12) || AES-256-GCM(key, IV, json) || tag(16)`. The firmware
  decrypts with mbedtls and rejects a wrong PIN (STATUS → `error: wrong PIN…`). A BLE sniffer without
  the PIN learns nothing. **The PIN is out-of-band** — set a unique `CAGI_PROV_PIN` per unit and print
  it on the device's label (the reference build defaults to `123456`).
- The `apiKey` is a **per-device, revocable** `cagi_` key the app mints for you. Revoke it on the
  API-keys page to instantly log the camera out.
- Optional hardware bonding: set `CAGI_BLE_REQUIRE_BONDING 1` to also require an encrypted,
  passkey-authenticated BLE link (`CAGI_BLE_PASSKEY`). Off by default because OS passkey dialogs are
  inconsistent across platforms — the app-layer AEAD already protects the secrets.
- The COMMAND characteristic (identify/reboot/factory-reset) is **unauthenticated** in v1: a local
  attacker in BLE range could reset the camera (a nuisance), but re-provisioning still requires the
  PIN, so they can't hijack it onto another account.
- TLS to the API uses `setInsecure()` by default (survives Cloudflare cert rotation; the API key
  still authenticates the device). Pin the ISRG Root X1 CA in `src/cloud.cpp` to harden.

## Factory reset

Write `factory-reset` to the COMMAND characteristic from the app (or call `Store::factoryReset()`).
It erases the `cagi` NVS namespace and reboots back into BLE-advertising mode. The firmware is
untouched.

## Layout

```
platformio.ini      build env (board, libs, partitions)
partitions.csv      dual-OTA, no-secure-boot flash map (re-flashable)
src/
  main.cpp          boot + connect/register/stream state machine
  config.h          version, BLE UUIDs (mirror core), camera pin map, NVS keys
  ble_prov.*        NimBLE provisioning GATT service
  cloud.*           Wi-Fi join + self-registration + frame/audio POST + control poll
  camera.*          OV2640 init + JPEG capture
  audio.*           mic (INMP441 I2S or PDM) init + WAV clip capture (optional; a capture task on S3)
  speaker.*         speaker: I2S TX, fetch, decode, play, results (optional, S3 only)
  clip.*            speaker clip decoder: WAV PCM16 / MP3 → mono PCM16 (plain C++, host-tested)
  third_party/      minimp3.h (CC0, github.com/lieff/minimp3 at ea99364f)
test/host/          clip_test.cpp: the clip decoder against good and bad clips, built with g++
  store.*           NVS credential storage
  status.*          shared lifecycle state + BLE notify
```

## License

[MIT](LICENSE).

## Build verification (2026-09-22)

All PlatformIO environments declared by this repository compiled successfully using PlatformIO
6.2.0. This verifies compilation, not physical wiring, sensor operation or live cloud connectivity.

## Build verification (2026-10-03, branch `cam-002-audio`)

PlatformIO 6.2.0 compiled `esp32cam`, `esp32cam-s3`, `esp32cam-verified`, both with
`-DCAGI_AUDIO_ENABLED=1` for the INMP441 path, and `hardware/CommandAGI-Cam-002/firmware`
(`esp32cam-s3` + mic + speaker). `test/host/clip_test.cpp` passed with g++ against built WAVs and
minimp3's layer III vectors:

```sh
g++ -std=c++17 -O1 -Wall -Isrc test/host/clip_test.cpp src/clip.cpp -o /tmp/clip_test && /tmp/clip_test [file.mp3 …]
```

This is compilation and a host test only. No board ran this firmware: the PDM mic, the capture task,
the speaker, the WebSocket messages and the video rate with the mic on are not verified on hardware.
