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

## Sealed stream: one writer, byte for byte

The sealing build (`-DCAGI_DEVICE_SEALS`) replaces the old verified-SKU capture manifest, which signed a
claim about each window but not the frames. Now the device signs the frames themselves: a seal's media
root covers the exact bytes of `video.mjpeg`, and each `cam-at` line names one frame's offset, length
and sha256. So the device must be the only writer of both, and the recorder must keep them byte for
byte. Two rules follow in `src/seal_runtime.cpp`:

- A frame is indexed only after `sendFrameJson` returned true, and sent only when its index line fits
  (`Stream::room`). A frame that was not sent leaves no line and no media bytes.
- The stamp (`t=` COM segment) is made on the device before the hash. Nobody stamps the frame again.

## Sealed stream: what a reboot costs

Seqs and seal counters must only increase, also across reboots, but NVS cannot take a write per line.
So `seal_runtime.cpp` reserves 4096 seqs and 1024 counters in NVS ahead of use, and a reboot skips the
rest of the block (a seq gap, which a reader reports). The media offset and the previous seal's hash
are not kept: after a reboot they start again at 0 and none. If the recorder continues the same stream
file, the verifier reports a break there. Fixing that needs the recorder to start a new file or to tell
the device where its file is; it is not the device's to guess.

## Sealed stream: the key and the board's locks

The key is made on the device and never leaves it: the serial port and BLE give out only the public
key. The default builds keep the "always re-flashable" rule, so the key sits in plain NVS and the seals
say `flash: plain`, `boot: unverified`. A production build needs secure boot v2, flash encryption in
release mode and NVS encryption (flash encryption alone leaves NVS in the clear). Those burn eFuses for
good and need the owner's signing key, so no env here turns them on.

## Audio on the ESP32-S3: two I2S ports, no camera conflict

The classic ESP32 camera driver uses I2S0 for pixel DMA, so the INMP441 mic must use I2S1 and nothing
else can use I2S. The S3 camera driver uses LCD_CAM (the esp32s3 `libesp32-camera.a` references
`LCD_CAM` and no I2S symbol). So on the DFR1154 the PDM mic takes I2S0 (PDM RX exists only there) and
the speaker takes I2S1. A speaker build on a classic ESP32 is a compile error.

## The mic task and the frame loop

`Audio::capture()` blocked the loop for a full clip (~1 s), and the camera sent no frame in that
time. On the S3 a capture task now owns the I2S reads and fills a PSRAM double buffer; the loop only
takes a finished clip. The buffer states (FREE → FILLING → READY → POSTING) change under one spinlock.
The classic ESP32 keeps the old blocking path, because that path is the one proven on hardware.

## Speaker: the socket, not HTTP

The speaker speaks the realtime socket's presence protocol (`play_audio`, `present_stop`,
`action_result`, `config.outputs`), the one host-core speaks, so the platform's grant check and
record-before-send apply unchanged. Only the loop's task writes to the socket; the speaker task hands
its results to the loop through a queue. links2004/WebSockets closes the socket on a message over
15 kB (`WEBSOCKETS_MAX_DATA_SIZE`, not overridable), so large clips must come by `url`.

## MP3 decoder: minimp3, vendored

The platform's voice is MP3. ESP8266Audio and the Helix wrappers are GPL or RPSL; this firmware is
MIT. minimp3 is CC0, one header, and builds unchanged on arduino-esp32 2.x. It keeps ~16 kB of
scratch on the stack, so the speaker task has a 40 kB stack. The task decodes the whole clip into
PSRAM before it reports `ok`, so `durationMs` is the real length and a bad clip fails before any
sound.

## Cellular: PPP needs an ESP-IDF build

arduino-esp32 2.x's precompiled lwIP has no PPP (`CONFIG_LWIP_PPP_SUPPORT` is off), and its `PPP` class
exists only in 3.x. TinyGSM would use the modem's own TCP/TLS stack, so HTTPS and the WebSocket client
would need other code. The cellular envs instead build Arduino as an ESP-IDF 4.4.7 component with
Arduino's own sdkconfig plus PPP, and feed the modem's UART into an esp_netif PPP interface. Everything
above lwIP is unchanged. The plain envs keep the precompiled build. Two traps: the sketch is
`firmware.cpp` (an IDF build that takes sources from outside the project names objects by basename, and
Arduino has a `main.cpp`), and the esp32-camera driver must come from the component registry.

## Cellular: what the firmware never does

It never retries a SIM PIN (three wrong PINs lock the SIM), never enables GSM (`AT+CNMP=38`: a GSM
burst draws more than the boards supply), and never logs an AT command (AT+CPIN carries the PIN).
