# Batch flashing + labels

Production tooling for flashing many ESP32-CAM units, each with a **unique provisioning PIN** printed
on its label. The PIN is the out-of-band secret that seals the BLE provisioning payload (see the main
[README](../README.md) → _Security notes_); shipping every unit with the same baked-in `123456` would
let anyone in BLE range re-provision a camera, so each board gets its own.

## `batch-flash.mjs`

Flash + label **one board at a time**. It mints a random 6-digit PIN, compiles it in via
`-DCAGI_PROV_PIN`, flashes over USB, then reads the board back over serial to capture the exact MAC
suffix it advertises (`CommandAGI Cam XXXX`) — so the printed label matches what the app shows during
pairing. Each unit is appended to `labels.csv` (the PIN ledger) and optionally rendered to a printable
`labels.html`.

```bash
cd apps/clients/firmware/esp32-cam

node tools/batch-flash.mjs                 # random PIN, auto-detect port
node tools/batch-flash.mjs --port /dev/ttyUSB0
node tools/batch-flash.mjs --pin 482913    # force a PIN instead of random
node tools/batch-flash.mjs --html          # also (re)write labels.html
node tools/batch-flash.mjs --html-only      # just rebuild labels.html from the CSV
```

Per board: plug it in (tie **GPIO0→GND** for the first flash on AI-Thinker boards), run the command,
wait for `✓ labeled`, unplug, plug in the next. The on-screen label block (Unit + PIN) is what to
print/stick on the unit; `labels.html` lays all of them out on a printable sheet.

Requires [PlatformIO](https://platformio.org/) (`pio`) on PATH; the MAC ledger column additionally
uses `esptool.py` (bundled with PlatformIO) but is best-effort — the **suffix** read over serial is
what the label needs.

> **`labels.csv` is a secret.** It maps each unit's PIN to its MAC. Keep it with the batch, hand it to
> whoever applies the labels, and do **not** commit it (it's gitignored). Treat it like a key list.
