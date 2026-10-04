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
before flashing.


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
- A **sealing** build (`-DCAGI_DEVICE_SEALS=1`, env `esp32cam-s3-seals`, and CommandAGI-Cam-002) signs
  its own frames with a key made on the device (§ Sealed stream).

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
>
> The one exception is deliberate and is not in this repository: CommandAGI-Cam-002's production envs
> (CommandAGI's `hardware/CommandAGI-Cam-002/firmware`) lock a unit for good at the factory, so that its
> sealing key cannot be copied (§ Sealed stream). A locked unit is not covered by this guarantee. Every
> env here stays open.

### Why it can ALWAYS be re-flashed (the guarantee)

The ESP32's serial **download mode lives in mask ROM** and runs _before_ the application on every
reset where GPIO0 is held low — so no firmware can ever block re-flashing. The only thing that can
permanently lock a board is **burning an eFuse** (Secure Boot, Flash Encryption, or the
download-disable bit), and this firmware does **none** of that:

- No eFuse is written anywhere in `src/`. The only `esp_efuse_*`, `esp_secure_boot_*` and
  `esp_flash_encrypt*` calls are reads in `src/chip_lock.cpp` (`esp_efuse_read_field_bit()`,
  `esp_secure_boot_enabled()`, `esp_flash_encryption_enabled()`, `esp_get_flash_encryption_mode()`): a
  sealing build reports them in its seals; they change nothing.
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
| INFO           | `…0002`     | read          | `{ kind, model, fw, hwid, name, provisioned, mic, spk?, secure, salt?, sealKey? }` |
| STATUS         | `…0003`     | read + notify | `{ state, ip?, sessionId?, deviceId?, error?, link?, operator?, rssi?, rsrp?, battery?, ir? }` |
| PROVISION      | `…0004`     | write         | PIN-sealed `{ ssid, psk, apiBaseUrl, apiKey, deviceName?, apn?, simPin? }` (see _Security_) |
| COMMAND        | `…0005`     | write         | `identify` \| `reboot` \| `factory-reset`                                    |

`state` walks `idle → wifi_connecting → registering → streaming` (with `wifi_failed` /
`register_failed` / `error` branches the app surfaces so you can retry). `paused` means online but
every sensor was turned off remotely (see _Remote sensor control_).

`link`, `operator`, `rssi`, `rsrp` come only from cellular builds and `battery` (`{ mv, pct?, charging? }`)
only from battery builds, and `ir` (`{ mode, on, lux?, night }`) only from IR builds, so the STATUS of other
builds does not change. `apn` and `simPin` are
optional: a payload without them stays valid, and the sealed format does not change. A cellular build
accepts an empty `ssid` and then uses only the modem.

## Cellular (`-DCAGI_CELLULAR_ENABLED=1`)

Both camera products carry a SIMCom A7670G LTE Cat-1 modem on a carrier or base board. The firmware
drives it over a UART and runs PPP into lwIP, so HTTPS, the realtime socket and TLS work over the modem
without change. Default is off (`0`).

| env | board | modem pins (`src/config.h`) |
| --- | --- | --- |
| `esp32cam-cellular` | AI-Thinker + Cam-001 base board | UART2: RX IO13, TX IO14; PWRKEY IO12; STATUS IO15; rail off IO2. No mic: IO13/14/15 are the INMP441's pins |
| `esp32cam-s3-cellular` | DFR1154 + Cam-002 carrier | UART1: TX GPIO43, RX GPIO44 (Gravity); PWRKEY GPIO11; STATUS GPIO12; RESET GPIO13 (microSD contacts) |

Pins come from `hardware/CommandAGI-Cam-001/README.md` and `hardware/CommandAGI-Cam-002/README.md`
(§ modem pin map). PWRKEY and RESET drive a transistor: HIGH = pressed or held.

**Build.** arduino-esp32 2.x ships lwIP without PPP. The cellular envs therefore build Arduino as an
ESP-IDF component (`framework = arduino, espidf`). `sdkconfig.defaults.<soc>` is Arduino's own
sdkconfig plus `CONFIG_LWIP_PPP_SUPPORT` (and octal PSRAM on the S3). The camera driver comes from the
ESP-IDF component registry (`src/idf_component.yml`, `espressif/esp32-camera` 2.0.4). The sketch file is
`src/firmware.cpp`, not `main.cpp`, because that build also compiles Arduino's own `main.cpp`.

**Modem sequence** (one task, core 0):

1. Power: PWRKEY high 100 ms, wait for STATUS (≤ 15 s), then `AT` at 115200 or the fast rate. A modem
   that was already on (an ESP32 reset does not reset it) gets `+++` and `ATH` first.
2. `ATE0`, `AT+CMEE=2`, `AT+IPR=921600` (falls back to 115200 if the fast rate fails; no RTS/CTS).
3. SIM: `AT+CPIN?`. If the SIM wants a PIN, the firmware sends the provisioned `simPin` once per boot.
   It never retries a rejected PIN (three wrong PINs lock the SIM); it reports `SIM PIN rejected`.
4. `AT+CNMP=38` (LTE only: a GSM burst draws more current than the boards supply), `AT+CGDCONT` with
   the provisioned `apn` (none = the SIM's default), then `AT+CEREG?` until registered (≤ 180 s).
5. Signal and operator: `AT+CSQ` (RSSI), `AT+CPSI?` (RSRP), `AT+COPS?`. AT is not available while PPP
   runs (no CMUX), so these values are from the moment before the dial; `signalAgeMs` says how old.
6. `ATD*99#`, then PPP. If the modem does not answer AT, the firmware pulses RESET (Cam-002) or cycles
   the rail (Cam-001), with a growing back-off up to 2 min.

**Link policy.** Wi-Fi when it is provisioned and connected, else cellular. With no link at all the
firmware tries Wi-Fi once (as before, ≤ 20 s), then brings up the modem while the Wi-Fi station keeps
reconnecting in the background. It hangs up PPP after Wi-Fi has been up for 60 s, and keeps the modem
registered for a fast fallback. lwIP routes by interface priority (Wi-Fi 100, PPP 20). A link change
restarts the realtime socket, because the old socket is bound to the old interface's address.

**Data budget.** A QVGA JPEG at quality 12 is about 10–20 kB. At the default 33 ms (30 fps) that is
about 1–2 GB per hour; the Cam-002 hardware notes give 0.9 GB/h. On cellular the firmware sends at
most one frame per `CAGI_CELLULAR_INTERVAL_MS` (default 1000 ms: about 36–72 MB per hour). The server can
set `cellularIntervalMs` in a control response, as it sets `intervalMs`. The mic adds 32 kB/s (about
115 MB per hour) while it is on; the operator can turn it off.

**Reported state.** STATUS (BLE) carries `link`, `operator`, `rssi` (dBm) and `rsrp` (dBm). On the
realtime socket the device sends `{"type":"status","status":"live","link":…,"operator":…,"rssi":…,
"rsrp":…,"signalAgeMs":…,"battery":{…}}` on connect, on a link change and every 60 s. The platform's
`status` handler reads `status` and ignores the other fields today.

**Secrets.** The SIM PIN is stored in NVS like the Wi-Fi password and goes only to the modem. The API
key goes only to the API host, over whichever link is up. Nothing new receives Wi-Fi credentials or keys.

## Battery (`-DCAGI_BATTERY_ADC_PIN=<gpio>`)

A board with a cell divider reports its voltage, an estimated percent (a resting Li-ion curve, not a
fuel gauge) and, with `-DCAGI_BATTERY_STAT_PIN`, whether it charges. It appears in BLE STATUS
(`battery`) and in the runtime `status` message. Below 2.5 V the firmware reports no battery: the Cam-002
no-battery variant fits only the lower resistor, so it reads 0 V. Cam-002-battery: GPIO10, divider 2.
The Cam-002 charger exposes no STAT pin, so `charging` is absent there.

## IR night mode (`-DCAGI_IR_ENABLED=1`, DFR1154 only)

The DFR1154 has four 940 nm IR LEDs and an ambient light sensor. The `esp32cam-s3` env (and so
`esp32cam-s3-cellular` and both Cam-002 products) turns IR night mode on. Default is off (`0`); another
board is a compile error, because its pins are not known.

| part | pins | source |
| --- | --- | --- |
| IR LEDs: SY7200A boost (U2), four MHP3528IRCT-D in series, R7 = 6.8 Ω | EN/PWM on GPIO47 (`CAGI_IR_PIN`) | DFR1154 Schematic v1.1 (net `IR_CT`); DFRobot's pin table "IR: Infrared illumination (IO47)" |
| light sensor LTR-308ALS-01 | I2C address `0x53` (part ID `0xB1`) on GPIO8/9, the camera's SCCB lines; INT on GPIO48 (not used) | DFRobot's pin table "ALS: LTR-308"; `DFRobot_LTR308` (`LTR308_ADDR 0x53`), used by `DFR1154_Examples` 5.1 |

**Modes.** The operator's mode comes in the control state as `sensors.ir`: `"auto"`, `"on"` or `"off"`.
A missing or unknown value is `auto`, so servers that send only `cam` and `mic` keep working. The device
does not keep the mode over a reboot; it starts in `auto` until the server says otherwise.

- `auto`: the firmware reads the light every 2 s (`CAGI_IR_READ_MS`). IR goes on below 5 lx
  (`CAGI_IR_ON_BELOW_LUX`) and off above 15 lx (`CAGI_IR_OFF_ABOVE_LUX`); between the two the state does
  not change. With no light reading, IR is off.
- `on` / `off`: the operator's choice, whatever the light.
- In every mode IR is off while the camera does not run: the operator turned the camera off, the device is
  `paused`, the camera failed, or the loop is not streaming (no link, a retry, not provisioned).

**Night tuning.** While IR is on the firmware sets the OV3660's night mode (`set_aec2`: register 0x3A00
bit 2, a longer exposure in low light) and a grey image (`set_special_effect` 2). The lens passes 940 nm,
so colours under IR are false. When IR goes off it sets both back. A camera re-init sets them again.

**Reported state.** BLE STATUS and the realtime `{"type":"status",…}` message carry
`"ir":{"mode":"auto","on":true,"lux":3.2,"night":true}`. `mode` is the operator's mode; `on` is the level
the firmware drives on GPIO47, not proof that the LEDs emit; `lux` is the last reading (absent when there
is none); `night` is true when the sensor accepted the night tuning. The socket message goes out at once
when `mode`, `on` or `night` changes, and every 60 s. BLE STATUS notifies on those changes and when the
light moves by more than 25 %.

**Heat.** R7 sets the LED current. If the SY7200A regulates FB at 0.2 V (the usual value for this kind of
boost LED driver; its datasheet was not read), the current is about 29 mA and each LED takes about
40 mW. DFRobot gives 75 mW per LED at most. So the firmware sets no duty limit. Measure the current on a
board.

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

## Sealed stream (`-DCAGI_DEVICE_SEALS=1`)

The camera seals its own stream, so a verifier can check which key recorded the frames and that nobody
changed, dropped, added or moved one afterwards. The format is CommandAGI's (`docs/integrity.md` in the
CommandAGI repository, § sealed streams). The sealer is `src/seal/seal.{h,c}`, a verbatim copy of
CommandAGI's `deployments/clients/seal-c` (that repository's `tests/workbench/seal-c.test.mjs` checks
the two are the same). Default is off (`0`). `esp32cam-s3-seals` and both CommandAGI-Cam-002 envs turn
it on; any env builds with it.

**The key.** On its first boot the device makes an Ed25519 key (32 random bytes from
`esp_fill_random()` while the radio is on; libsodium signs) and keeps it in NVS, namespace `cagi-seal`.
A factory-reset does not erase it: the key is the unit's identity, not the owner's. In every env here
the key is in plain flash: anyone who holds the board can copy it. A CommandAGI-Cam-002 production unit
is locked at the factory before the key is made (below, the production lock).

**The status.** Each seal's `status` says what the chip reports at boot (`src/chip_lock_status.h`, canonical
JSON, keys sorted, because the signature covers the canonical form):

| field | values | read from | what it shows when locked |
| --- | --- | --- | --- |
| `boot` | `verified`, `unverified` | `esp_secure_boot_enabled()` | the ROM and the bootloader run only signed firmware |
| `flash` | `release`, `development`, `plain` | `esp_flash_encryption_enabled()`, `esp_get_flash_encryption_mode()` | flash is ciphertext; release: no plaintext reflash, no readout |
| `nvs` | `encrypted`, `plain` | `CONFIG_NVS_ENCRYPTION` and an `nvs_keys` partition that flash encryption covers | the key in NVS is ciphertext |
| `jtag` | `off`, `on` | eFuses `HARD_DIS_JTAG` and `DIS_USB_JTAG` (ESP32: `DISABLE_JTAG`) | no debugger reads RAM |
| `dl` | `secure`, `off`, `open` | eFuses `ENABLE_SECURITY_DOWNLOAD`, `DIS_DOWNLOAD_MODE` | the ROM loader cannot read flash |
| `fw` | the firmware version | `CAGI_FW_VERSION` | — |

The status is the device's own report, signed by the key it protects. It is not forged in transit, but
firmware that lies would sign a lie: a verifier believes it only as far as it believes the key's
certificate, and the factory that checked the same eFuses with the ROM's own report before it certified
the key. None of it says what the sensor saw.

**What it writes.** Two channels, both declared `seals: "device"` in the `channels` message:

| channel | medium | what |
| --- | --- | --- |
| `cam` | video | `video.mjpeg`: each frame, stamped first: a JPEG COM segment `t=<ISO time>` right after SOI (CommandAGI's `mjpeg.js` `stampFrame`). `records.jsonl`: only its seals |
| `cam-at` | records (`byte-ranges`) | one line per frame sent, `{"t","seq","src":"device","kind":"event","frame":{"offset","length","sha256"}}` (the frame's bytes in `cam`'s `video.mjpeg`), and its seals |

About once a second each stream gets a seal line in its own `records.jsonl`: `cam`'s covers the
`video.mjpeg` bytes since its last seal, `cam-at`'s the index lines since its last seal (RFC 9162 Merkle
roots, 64 KiB media chunks). One key signs both, and one counter numbers both. A seal names the
contract log's head, `"log":{"head":"<64 hex>","seq":N}`, fetched from `GET <api>/public/contract/head`
every 10 s. Once a minute the platform appends a recent block of its chain to that log (a beacon), and
every later head hashes it, so the frames after the seal were made after that block. The device reads no
chain: one chain read a minute serves every device. Its
`clock` is `ntp` when SNTP set the clock before every frame it covers, else `none`. The first seal of
each stream after a boot announces the key: the factory's certificate chain when the unit has one, else
the bare key (`spki`). Seqs and seal counters never go back, also across a reboot: the device reserves
them in NVS 4096 seqs and 1024 counters at a time, so a reboot leaves a gap.

**The wire** (the realtime socket). A frame is sent only when it can be indexed, and it is indexed only
when it was sent:

```json
{"type":"frame","channelId":"cam","kind":"camera","url":"data:image/jpeg;base64,<the stamped frame>"}
{"type":"data","channelId":"cam","kind":"camera","format":"jsonl","url":"data:text/plain;base64,<cam's seal lines, each ending in \n>"}
{"type":"data","channelId":"cam-at","kind":"events","format":"jsonl","url":"data:text/plain;base64,<cam-at's lines, each ending in \n>"}
```

A sealing build does not send bare binary frames (the platform does not record those). Each stream's
lines wait in a 32 kB outbox until the socket takes them, once a second with the seals; while the index
outbox is full, no frame is sent. `cam-at`'s lines go out only after `cam`'s seal over their frames.

**Resume.** The recorder keeps a group (the lines and frames since a seal, and the seal) only when it
extends the file it holds; it cannot repair what the device signed. When it keeps none of a group (a
frame lost after `sendTXT` returned), and whenever the socket connects, it says where its files stand:

```json
{"type":"seal_resume","reason":"…","streams":[
  {"channelId":"cam","seq":1031,"prev":"<sha256 of the file's last seal line>","counter":2061,"file":"video.mjpeg","bytes":4718592},
  {"channelId":"cam-at","seq":4130,"prev":"…","counter":2062}]}
```

The device drops everything not yet sealed and sent, from both streams (an index line names its frame's
bytes), and continues after the files: the next seq after each file's last, the next seal chained to the
file's last seal and announcing the key again, the frames at the file's length. Seqs and the counter
never go back. It answers `{"type":"seal_resumed","channelIds":["cam","cam-at"]}` before it sends
another frame; the recorder drops what comes before that. A reboot, a lost link or a lost frame is then a
gap the files show (skipped seqs, a jump in time), and the seals still verify.

**A frame on demand.** A principal with a standing grant (`open` on the unit's `cam`) asks the platform
for a frame now. The platform records the request, then sends:

```json
{"type":"frame_request","channelId":"cam","requestId":"fr-…","expedite":false}
```

The device takes one request at a time (`Seal::onMessage` keeps it; the main loop serves it in
`Seal::serveFrameRequest`, not inside the socket's callback). It captures a frame at once, sends it and
its index line, seals both streams at once (not at the next second), sends the seal lines, and then
answers with `cam`'s seal over the frame, its seq and the sha256 of its line:

```json
{"type":"frame_sealed","requestId":"fr-…","channelId":"cam","seq":1032,"hash":"<64 hex>"}
```

When it does not capture, it says why instead: `{"type":"frame_sealed","requestId","channelId","refused":"the
camera is off"}` (also: the sensor returned no frame, the socket did not take the frame, the seal lines did
not go out yet, another request is waiting, the channel is not `cam`). The recorder checks that the seal
the answer names is one it kept. `expedite` asks the platform to anchor the seals on chain at once; the
device does nothing else for it. `DeviceSeal::frameAnswer` writes the answer; the host test checks it.

**The certificate.** The factory certifies the key with CommandAGI's `scripts/integrity/device-ca.mjs`
(`--key software --envelope none`, and `--lock none` or `efuse` for Cam-002). CommandAGI's
`scripts/integrity/factory-unit.mjs` runs these steps for one unit and writes its provisioning record;
the production CA certifies only from a record that shows the lock. By hand, for a dev unit, on the
bench before the unit has creds:

```sh
python3 tools/seal-provision.py spki /dev/ttyACM0 > unit.spki.pem
node scripts/integrity/device-ca.mjs issue --env dev --spki unit.spki.pem --product CommandAGI-Cam-002-no-battery \
  --serial <serial> --key software --envelope none --lock none > unit.chain.pem   # in the CommandAGI repository
python3 tools/seal-provision.py chain /dev/ttyACM0 unit.chain.pem
```

The serial commands are `seal-spki` (the key as PEM), `seal-chain <JSON array of base64 DER, leaf
first>` (refused unless the leaf holds this unit's key), `seal-chain-clear`, `seal-status` and
`prov-pin <6 digits>`. BLE INFO also carries the key (`sealKey`, base64url SPKI). No command reads the
private key, and none can be made to: the firmware has no code that prints or sends it.

**The production lock** (CommandAGI-Cam-002's `-production` envs; CommandAGI's
`hardware/CommandAGI-Cam-002/README.md` § integrity has the decision and what it does and does not
protect against). The build adds secure boot v2, flash encryption in release mode, NVS encryption,
secure download mode, anti-rollback and no core dump; its partition table adds an encrypted `nvs_keys`
partition. The factory (CommandAGI's `scripts/integrity/factory-unit.mjs`) burns the two firmware-signing
key digests; the signed bootloader burns the rest on the first boot; then the app makes the NVS keys
and the sealing key, inside the locked chip. Such a build is one signed image for every unit, so it
cannot compile a per-unit PIN in: with `-DCAGI_PROV_PIN_NVS=1` the PIN comes from NVS (`cagi-fac`, which a
factory-reset keeps), written once by `prov-pin` (a second `prov-pin` is refused), and a unit with no PIN
refuses every PROVISION write. On the bench, before the unit has creds:

```sh
python3 tools/seal-provision.py status /dev/ttyACM0 --wait 300   # waits through the first boot's encryption
python3 tools/seal-provision.py pin    /dev/ttyACM0 482913
```

**Not done.**

- No board ran it. The host test (`test/host/seal_test.cpp`: the streams, a refused second, a resume and a
  frame request with its answer) and the builds are all that was checked. No unit answered a real
  `frame_request` from the deployed platform.
- The production lock has not run on a board: no eFuse was burned. The builds compile and are signed with a
  throwaway key pair, and the factory script runs against a fake board only.
- No over-the-air update: a locked unit cannot be updated until the firmware has one (it must verify the
  new image, which `CONFIG_SECURE_SIGNED_ON_UPDATE` does, and revoke a leaked key's digest slot).
- No unit has streamed to the deployed recorder. The host test runs this sealing code through a refused
  second and a resume, and CommandAGI's recorder check (`checkSealGroup`) accepts every group it sent.

## Remote sensor control

While streaming, each frame/audio POST response carries the operator's desired sensor state
(`{ sensors: { cam, mic, ir? } }`), so toggling a camera's cam/mic off in the dashboard stops that stream
within one interval. When every sensor is off the device goes `paused` and polls
`GET /v1/streams/:sessionId/:deviceId/control` every few seconds, so it can be turned back on
remotely (it never needs a power-cycle to resume).

### Security notes

- **The PROVISION payload is PIN-encrypted.** With `CAGI_PROV_SECURE` (default on), INFO advertises
  `secure: true` + a per-boot `salt`, and the app must seal the payload: `key = HKDF-SHA256(PIN, salt,
"cagi-cam-prov-v1")`, `wire = IV(12) || AES-256-GCM(key, IV, json) || tag(16)`. The firmware
  decrypts with mbedtls and rejects a wrong PIN (STATUS → `error: wrong PIN…`). A BLE sniffer without
  the PIN learns nothing. **The PIN is out-of-band** — set a unique `CAGI_PROV_PIN` per unit and print
  it on the device's label (the reference build defaults to `123456`). A build with
  `-DCAGI_PROV_PIN_NVS=1` has no PIN compiled in: the factory writes it once (`prov-pin`, § Sealed stream).
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
untouched, and so is the sealing key (`cagi-seal`).

## Layout

```
platformio.ini      build env (board, libs, partitions)
partitions.csv      dual-OTA, no-secure-boot flash map (re-flashable)
src/
  firmware.cpp      boot + link + connect/register/stream state machine (setup/loop; not main.cpp:
                    the ESP-IDF build of the cellular envs also compiles Arduino's cores/esp32/main.cpp)
  config.h          version, BLE UUIDs (mirror core), camera pin map, NVS keys
  ble_prov.*        NimBLE provisioning GATT service
  cloud.*           Wi-Fi join + self-registration + frame/audio POST + control poll
  camera.*          OV2640 init + JPEG capture
  audio.*           mic (INMP441 I2S or PDM) init + WAV clip capture (optional; a capture task on S3)
  speaker.*         speaker: I2S TX, fetch, decode, play, results (optional, S3 only)
  cellular.*        LTE modem: power, AT setup, PPP into lwIP (optional, cellular envs)
  battery.*         cell voltage / percent / charging (optional)
  seal/             seal.h, seal.c: CommandAGI's deployments/clients/seal-c, verbatim (plain C)
  device_seal.*     the sealed camera streams: stamp, index, seal, outboxes (plain C++, host-tested)
  seal_runtime.*    the key in NVS, libsodium Ed25519, SNTP, the block, the serial commands (optional)
  chip_lock.*       the chip's lock as it reports it (read-only), the PIN from NVS, `prov-pin`
  chip_lock_status.h  the seal's status object from the lock's facts (plain C++, host-tested)
  ir.*              IR night mode: GPIO47, the LTR-308, the night tuning (optional, DFR1154)
  ir_policy.h       the IR decision with hysteresis (plain C++, host-tested)
  CMakeLists.txt    the ESP-IDF main component (cellular envs only)
  idf_component.yml esp32-camera from the ESP-IDF registry (cellular envs only)
CMakeLists.txt      the ESP-IDF project file (cellular envs only)
sdkconfig.defaults.*  Arduino's sdkconfig + PPP, per SoC (cellular envs only)
  clip.*            speaker clip decoder: WAV PCM16 / MP3 → mono PCM16 (plain C++, host-tested)
  third_party/      minimp3.h (CC0, github.com/lieff/minimp3 at ea99364f)
test/host/          clip_test.cpp: the clip decoder against good and bad clips, built with g++;
                    seal_test.cpp: the sealed streams, verified by CommandAGI's JavaScript verifier;
                    ir_policy_test.cpp: the IR decision against a light level that crosses the thresholds
tools/              batch-flash.mjs, read-suffix.py (labels); seal-provision.py (the key's certificate)
  store.*           NVS credential storage
  status.*          shared lifecycle state + BLE notify
```

## License

[MIT](LICENSE).

## Build verification (2026-09-22)

All PlatformIO environments declared by this repository compiled successfully using PlatformIO
6.2.0. This verifies compilation, not physical wiring, sensor operation or live cloud connectivity.

## Build verification (2026-10-03, branches `cam-002-audio` and `cellular`)

PlatformIO 6.2.0 compiled `esp32cam`, `esp32cam-s3`, `esp32cam-verified`, `esp32cam-cellular`,
`esp32cam-s3-cellular`, `esp32cam` and `esp32cam-verified` with `-DCAGI_AUDIO_ENABLED=1` (the INMP441
path), `hardware/CommandAGI-Cam-001/firmware`, and `hardware/CommandAGI-Cam-002/firmware`
(`commandagi-cam-002`, `commandagi-cam-002-battery`: S3 + mic + speaker + cellular, + battery). `test/host/clip_test.cpp` passed with g++ against built WAVs and
minimp3's layer III vectors:

```sh
g++ -std=c++17 -O1 -Wall -Isrc test/host/clip_test.cpp src/clip.cpp -o /tmp/clip_test && /tmp/clip_test [file.mp3 …]
```

This is compilation and a host test only. No board ran this firmware: the PDM mic, the capture task,
the speaker, the WebSocket messages, the video rate with the mic on, the modem sequence, PPP, the
`AT+CPSI?` RSRP field order and the battery reading are not verified on hardware.

## Build verification (2026-10-03, branch `integrity-seals`)

PlatformIO 6.2.0 compiled `esp32cam`, `esp32cam-s3`, `esp32cam-s3-seals`, `esp32cam-cellular`,
`esp32cam-s3-cellular`, `esp32cam` with `-DCAGI_DEVICE_SEALS=1`, and
`hardware/CommandAGI-Cam-002/firmware` (`commandagi-cam-002`, `commandagi-cam-002-battery`, both with
the sealed stream). The sealed stream adds about 137 kB of flash (most of it libsodium) and 4.8 kB of
static RAM; the two sealers' state (2 x 49 kB) and the two outboxes (2 x 32 kB) are in PSRAM.
`test/host/seal_test.cpp` passed in CommandAGI's `tests/workbench/seal-c.test.mjs`: `cam`'s seals with
its `video.mjpeg`, and `cam-at`'s lines, verify in JavaScript; every index line names the bytes and the
stamp of its frame; a lost frame or a changed line breaks a seal. No board ran this firmware.
## Build verification (2026-10-04, branch `cellular`, IR night mode)

PlatformIO 6.2.0 compiled `esp32cam`, `esp32cam-s3` and `esp32cam-s3-cellular`, and
`esp32cam-s3-cellular` with the Cam-002 flags (`-DCAGI_AUDIO_ENABLED=1 -DCAGI_SPEAKER_ENABLED=1
-DCAGI_BATTERY_ADC_PIN=10`). Flash use before → after: `esp32cam` 1,325,457 → 1,325,457 bytes (the same
`firmware.bin`, 1,332,032 bytes: IR is compiled out); `esp32cam-s3` 1,178,761 → 1,184,981; `esp32cam-s3-cellular`
1,234,249 → 1,237,809. The host test passed:

```sh
g++ -std=c++17 -O1 -Wall -Isrc test/host/ir_policy_test.cpp -o /tmp/ir_policy_test && /tmp/ir_policy_test
```

This is compilation and a host test only. No board ran it: the LTR-308 reads on the shared SCCB port, the
lux values, GPIO47 driving the LEDs, the LED current and the OV3660 night tuning are not verified.

## Build verification (2026-10-04, `main`: the sealed stream and IR night mode merged)

PlatformIO 6.2.0 compiled every env on the merged tree: `esp32cam` 1,325,649 bytes of flash, `esp32cam-s3` 1,185,177,
`esp32cam-s3-seals` 1,327,941, `esp32cam-cellular` 1,384,065, `esp32cam-s3-cellular` 1,237,993, and
`hardware/CommandAGI-Cam-002/firmware`'s `commandagi-cam-002` 1,427,889 and `commandagi-cam-002-battery` 1,438,693.
`test/host/ir_policy_test.cpp` passed, and CommandAGI's `tests/workbench/seal-c.test.mjs` passed over
`test/host/seal_test.cpp`. No board ran it.

## Build verification (2026-10-04, branch `production-lock`: the chip's lock in the status, the PIN from NVS)

PlatformIO 6.2.0 (espressif32 6.13.0, ESP-IDF 4.4.7, esptool 4.11.0) compiled every env: `esp32cam` 1,325,917 bytes
of flash, `esp32cam` with `-DCAGI_DEVICE_SEALS=1` 1,471,661, `esp32cam-s3` 1,185,421, `esp32cam-s3-seals` 1,328,929,
`esp32cam-cellular` 1,384,153, `esp32cam-s3-cellular` 1,238,221, and CommandAGI's `hardware/CommandAGI-Cam-002/firmware`:
`commandagi-cam-002` 1,428,853, `commandagi-cam-002-battery` 1,439,657, and the two production envs 1,429,377 and
1,440,177, signed with a throwaway key pair made for the build (signed app 1,445,888 bytes, signed bootloader 32,768).
`test/host/ir_policy_test.cpp` passed, and CommandAGI's `tests/workbench/seal-c.test.mjs` passed over
`test/host/seal_test.cpp`, whose seals now carry a dev unit's status and a locked unit's; with the status keys out of
order, the test fails. No board ran it, and no eFuse was burned.

## Build verification (2026-10-04, branch `beacon-frames`: the log head and the frame request)

PlatformIO 6.2.0 compiled every env: `esp32cam` 1,325,649 bytes of flash (unchanged: the seals are compiled out),
`esp32cam-s3` 1,185,173, `esp32cam-s3-seals` 1,329,433, `esp32cam-cellular` 1,384,077, `esp32cam-s3-cellular`
1,238,005, and `hardware/CommandAGI-Cam-002/firmware`'s `commandagi-cam-002` 1,429,393 and
`commandagi-cam-002-battery` 1,440,189. CommandAGI's `tests/workbench/seal-c.test.mjs` passed over
`test/host/seal_test.cpp`: the seals name the log's head, and the frame request's answer names the seal over its
frame, which verifies in JavaScript. No board ran it.
