# Hardware

Schematics + wiring for the CommandAGI camera, authored in [tscircuit](https://tscircuit.com) (React
for circuit boards — the schematic lives in version control as code, diffable like the firmware).

## Bill of materials

| Part                                           | ~Cost | Notes                 |
| ---------------------------------------------- | ----- | --------------------- |
| AI-Thinker ESP32-CAM (ESP32 + OV2640, PSRAM)   | ~$6   | the camera + brains   |
| INMP441 I2S MEMS microphone breakout           | ~$2   | optional — adds audio |
| USB-UART adapter (CP2102/FTDI) or ESP32-CAM-MB | ~$2   | first flash only      |

The mic is **optional**: the firmware streams camera-only without it, mic-only if the camera fails,
and both when both are present.

## Sealed cameras

The verified-camera SKU (a secure element, a tamper switch, LiDAR, thermal and EMI sensors, a signed
capture manifest) is withdrawn: its design, its env and its stub drivers are gone. A camera now seals
its own frames (README § Sealed stream). CommandAGI's products carry the hardware for it:
CommandAGI-Cam-002 (a software key on the ESP32-S3) and CommandAGI-Cam-003 (a key in a security MCU
inside a tamper mesh), in the CommandAGI repository's `hardware/`.

## Files

- [`inmp441-wiring.circuit.tsx`](inmp441-wiring.circuit.tsx) — the INMP441 ↔ ESP32-CAM I2S wiring,
  matching the `CAGI_I2S_*` pin map in [`../src/config.h`](../src/config.h).

## Rendering

```bash
# one-off, no install:
npx tsci dev hardware/inmp441-wiring.circuit.tsx     # opens the interactive schematic/PCB viewer
npx tsci export png hardware/inmp441-wiring.circuit.tsx

# or add the dev dep (see package.json) and use the local CLI.
```

## Wiring quick reference

| INMP441 | ESP32-CAM | config.h           |
| ------- | --------- | ------------------ |
| VDD     | 3V3       | —                  |
| GND     | GND       | —                  |
| L/R     | GND       | left channel       |
| WS      | GPIO15    | `CAGI_I2S_WS_PIN`  |
| SCK     | GPIO14    | `CAGI_I2S_SCK_PIN` |
| SD      | GPIO13    | `CAGI_I2S_SD_PIN`  |
