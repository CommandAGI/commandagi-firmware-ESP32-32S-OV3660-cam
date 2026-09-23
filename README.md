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
| INFO           | `…0002`     | read          | `{ kind, model, fw, hwid, name, provisioned, mic, secure, salt? }`           |
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
  audio.*           INMP441 I2S mic init + WAV clip capture (optional)
  store.*           NVS credential storage
  status.*          shared lifecycle state + BLE notify
```

## License

[MIT](LICENSE).

## Build verification (2026-09-22)

All PlatformIO environments declared by this repository compiled successfully using PlatformIO
6.2.0. This verifies compilation, not physical wiring, sensor operation or live cloud connectivity.
