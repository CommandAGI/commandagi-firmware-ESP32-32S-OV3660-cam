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

## Verified-camera SKU (multi-modal + tamper-evident)

A **separate, higher-tier board** that produces cryptographically stronger provenance: it holds its
signing key in a secure element, records enclosure tamper, and corroborates each capture with
independent sensors so a screen/print replay is far harder to pass. Built with the
`[env:esp32cam-verified]` PlatformIO env (see [`../platformio.ini`](../platformio.ini)); each sensor is
independently compiled in via a `-DCAGI_SENSOR_*` flag, so a unit only carries what it populates.

> **Full EE design → [`VERIFIED_SKU.md`](VERIFIED_SKU.md)** — the buildable hardware/electronics design
> review: block diagram, MCU justification, the **secure-element part constraint** (the platform verifies
> Ed25519, so the ATECC608 is out — use an NXP SE050), sensor subsystems, tamper tiers (evident vs
> responsive), power budget, PCB stack-up, full BOM + per-unit cost, and the manufacturing/attestation
> flow. Read it before a hardware bring-up.

| Part                                               | ~Cost | Modality / role                                                                                                                                                                                              | config                                                               |
| -------------------------------------------------- | ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------- |
| ESP32-S3 (8MB PSRAM, 16MB flash) + camera          | ~$12  | `rgb` — the base capture + brains                                                                                                                                                                            | `-DCAM_BOARD_ESP32S3`                                                |
| **NXP SE050** Ed25519 secure element (I2C)         | ~$1.5 | holds the Ed25519 signing key; key never leaves silicon; carries the factory attestation cert. **NOT the ATECC608** — that is P-256 only and cannot sign what the platform verifies (see VERIFIED_SKU.md §3) | `-DCAGI_SECURE_ELEMENT=1`, `CAGI_SE_I2C_ADDR=0x48`                   |
| Chassis tamper switch (NC) or conductive mesh      | ~$1   | breaks a normally-closed loop when the case is opened → latched `caseIntact=false` (tamper-EVIDENT)                                                                                                          | `-DCAGI_TAMPER_ENABLED` (default on for the SKU), `CAGI_TAMPER_GPIO` |
| Solid-state LiDAR / ToF (VL53L5CX 8×8, I2C)        | ~$6   | `lidar` — depth frame; a flat plane ⇒ screen replay                                                                                                                                                          | `-DCAGI_SENSOR_LIDAR=1`                                              |
| Thermal IR array (MLX90640 32×24, I2C)             | ~$25  | `ir` — thermal frame; uniform panel temp ⇒ display                                                                                                                                                           | `-DCAGI_SENSOR_THERMAL=1`                                            |
| EMI/flicker front-end (E-probe + photodiode → ADC) | ~$1.5 | `emi` — display refresh-rate EM + optical flicker ⇒ replay                                                                                                                                                   | `-DCAGI_SENSOR_EMI=1`                                                |

The firmware signs a per-window `CaptureManifest` (`{seq, fw, bootState, caseIntact, modalities,
selfCoherence, signer, signature}` — see [`../src/manifest.cpp`](../src/manifest.cpp)) and streams it as
a sidecar next to the JPEG frames. The **platform re-verifies** the signature and **recomputes**
cross-modal coherence from the actual frames (`packages/domain/core/src/capture-coherence.ts`) — the device
never certifies its own integrity score.

### Tamper-EVIDENT, not tamper-PROOF

The tamper switch **detects and records** that the enclosure was opened; it does not stop a determined
attacker with physical possession. On a non-SE build the latch is an NVS flag (best-effort — a reflash
can clear it). On the SE build the latch is mirrored into the always-on RTC/coin-cell domain so an open
while powered off is still caught, and the signed `CaptureManifest` reports `caseIntact=false` from then
on — the platform applies `TAMPER_DISCOUNT` (`packages/domain/core/src/integrity.ts`) and the device-tier source
is effectively dead.

**Be honest about the limit (VERIFIED_SKU.md §5.3):** a ~$1 SE (SE050/ATECC-class) has **no autonomous
active-mesh tamper input** that zeroizes a key on intrusion — that is HSM-grade hardware. So the shipped
tier is tamper-**EVIDENT** (detected + recorded + scored down), not tamper-**PROOF**. An optional
"responsive" tier adds a lid + SE-zone conductive mesh and a firmware-mediated SE key-delete on the next
powered boot — best-effort, defeated by an attacker who cuts power and reads the die offline. True
tamper-proofing (a powered mesh envelope that zeroizes independent of firmware) is out of scope for this
board, and INTEGRITY.md §3.2 deliberately does not ask for it.

### Re-flashability preserved

Like the stock board, the verified SKU does **NOT** burn Secure Boot / Flash Encryption eFuses
(`docs/platform/CAMERAS.md` requires the board stay USB-re-flashable). Key confidentiality comes from
the secure element, not from locking the board.

## Files

- [`VERIFIED_SKU.md`](VERIFIED_SKU.md) — the full verified-camera SKU hardware/EE design (architecture,
  MCU, the Ed25519 secure-element choice, sensors, tamper tiers, power, PCB, BOM, provisioning).
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
