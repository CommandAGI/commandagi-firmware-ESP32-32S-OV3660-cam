# Verified-Camera SKU — hardware + electronics engineering design

> **Scope.** This is the buildable hardware/EE design for the CommandAGI _verified camera_ — a
> multi-modal, tamper-evident capture device whose signing key lives in a secure element and whose
> cross-modal sensor stack makes a screen/print replay measurably hard. It is the physical half of the
> integrity contract in ``docs/trust/INTEGRITY.md`` (platform-internal reference); the platform
> (scoring, coherence recompute, attestation verify) is the other half. The stock ~$6 AI-Thinker board
> ([`../README.md`](../README.md)) is unchanged — this is a separate, higher-tier SKU built from the
> `[env:esp32cam-verified]` PlatformIO env.
>
> **Status.** Reference design / EE review draft. The firmware capture path (signed `CaptureManifest`
> sidecar, tamper latch, sensor HAL stubs) exists behind `CAGI_VERIFIED_SKU`; this doc is what a
> hardware team fabricates and brings up against. It is deliberately honest about what is tamper-EVIDENT
> vs tamper-RESPONSIVE, and about the one hard cryptographic constraint (§3) that forces the secure-
> element part choice.

**The one non-negotiable the whole design turns on:** the platform verifies **Ed25519**
(`verifyFrameChain` / `verifyManifest` in ``packages/domain/core/src/integrity.ts`` (platform-internal reference)
use `@noble/ed25519`). The signing key MUST produce Ed25519 signatures over the exact domain-separated
pre-images `"cagi-frame:v1|…"` and `"cagi-manifest:v1|…"`. That single fact rules out the "obvious"
ATECC608 (ECDSA P-256 only) as a drop-in and dictates the secure element in §3.

---

## Table of contents

1. [System architecture](#1-system-architecture)
2. [MCU choice — ESP32-S3](#2-mcu-choice--esp32-s3)
3. [The secure element (the critical constraint)](#3-the-secure-element--the-critical-constraint)
4. [Sensor subsystems](#4-sensor-subsystems)
5. [Tamper subsystem — case-open detection (honest about limits)](#5-tamper-subsystem--case-open-detection)
6. [Power](#6-power)
7. [PCB](#7-pcb)
8. [Full BOM + per-unit cost](#8-full-bom--per-unit-cost)
9. [Manufacturing, provisioning & attestation flow](#9-manufacturing-provisioning--attestation-flow)
10. [Enclosure & mechanical](#10-enclosure--mechanical)
11. [Threat-model → hardware-defense → score mapping](#11-threat-model--hardware-defense--score-mapping)
12. [Variants & roadmap](#12-variants--roadmap)

---

## 1. System architecture

Four domains — compute, sensor, security, power — meet at the ESP32-S3. Data flows sensors → MCU →
per-frame Ed25519 hash chain + a per-window signed `CaptureManifest`, out over Wi-Fi (streaming) and
BLE (provisioning only).

```
                          CommandAGI Verified Camera — block diagram
  ┌───────────────────────────────────────────────────────────────────────────────────────────┐
  │  SENSOR DOMAIN                                COMPUTE DOMAIN            SECURITY DOMAIN       │
  │                                                                                             │
  │  ┌───────────────┐  DVP 8-bit + SCCB   ┌─────────────────────────┐                          │
  │  │ OV5640 RGB    │────────────────────▶│  ESP32-S3 (dual LX7)     │   I2C-B (400 kHz)        │
  │  │ 2592×1944     │  XCLK 20 MHz        │  ┌───────────────────┐   │  ┌────────────────────┐  │
  │  └───────────────┘                     │  │ Core0: capture +  │   │  │ NXP SE050 (Ed25519)│  │
  │                                        │  │  JPEG + WS TX     │   │  │  key in silicon    │  │
  │  ┌───────────────┐  I2C-A (1 MHz)      │  ├───────────────────┤   │◀─┤  attestation cert  │  │
  │  │ VL53L5CX ToF  │───┐                 │  │ Core1: sensors +  │◀──┼──┤  P1.02 = ENA        │  │
  │  │ 8×8 depth     │   │                 │  │  Ed25519 chain +  │   │  └────────────────────┘  │
  │  └───────────────┘   │                 │  │  manifest sign    │   │                          │
  │  ┌───────────────┐   ├─ SDA-A / SCL-A  │  └───────────────────┘   │  ┌────────────────────┐  │
  │  │ MLX90640 IR   │───┤  (shared)       │   8 MB PSRAM (OPI)       │  │ Tamper latch       │  │
  │  │ 32×24 thermal │   │                 │   16 MB flash (dual-OTA) │  │  NC mesh loop →     │  │
  │  └───────────────┘   │                 │                          │  │  GPIO7 + RTC domain│  │
  │  ┌───────────────┐   │                 │   ADC1 (E-field/flicker) │  │  + coin-cell backup│  │
  │  │ EMI front-end │───┘ ADC1_CH0/CH3    │◀─────────────────────────┼──┤  (always-on)       │  │
  │  │ E-probe+photo │                     │                          │  └────────────────────┘  │
  │  └───────────────┘                     └────────────┬─────────────┘                          │
  │                                                     │ Wi-Fi 2.4G (WS frames + manifest)       │
  │                                                     │ BLE (provisioning only, then off)       │
  └─────────────────────────────────────────────────────┼──────────────────────────────────────┘
                                                        ▼
              POST/WS → api.commandagi.com  → SignedFrame chain + CaptureManifest sidecar
                                            → platform re-verifies sig, RECOMPUTES coherence

  POWER DOMAIN:  USB-C (5V/PD) ─▶ 3V3 buck (SoC/sensors) ─▶ 1V8 LDO (SE050 optional)
                               └▶ Li-ion charger (opt.) ─▶ battery ─▶ boost/PPM
                 Coin cell (CR2032) ─▶ VBAT_RTC (RTC domain + tamper latch, always-on)
```

**Data flow, precisely:**

1. **Capture.** Core0 pulls a JPEG from the OV5640 over the parallel DVP bus. Core1 concurrently reads
   the depth (VL53L5CX), thermal (MLX90640) and EMI (ADC) frames — these are _low-rate_ (1–16 Hz),
   the RGB is the bandwidth driver.
2. **Chain.** For each streamed RGB frame, Core1 computes `frameHash = sha256(jpeg_bytes)`, forms the
   signing message `"cagi-frame:v1|seq|prevHash|frameHash"`, and asks the secure element to Ed25519-sign
   it. `prevHash(n) = sha256(signing_message(n-1))`; `prevHash(0) = sha256("cagi-frame-genesis:v1")`.
   The `SignedFrame{seq, prevHash, frameHash, signer, signature}` rides the stream as a structured
   sidecar (no new file type — the integral/unit model of INTEGRITY.md §4).
3. **Manifest.** Every `CAGI_MANIFEST_INTERVAL_MS` (2 s) Core1 builds a `CaptureManifest{seq, fw,
bootState, caseIntact, modalities, selfCoherence}`, SE-signs it over `"cagi-manifest:v1|…"`, and
   emits it on the `manifest` sidecar channel. This binds firmware/boot state + the live modality set +
   the tamper reading to the frame segment.
4. **Egress.** Frames + manifest go out over the persistent Wi-Fi WebSocket. BLE is used only during
   provisioning and is then shut down (the classic-radio coexistence + throughput reason in
   `config.h`; on S3 the radios are independent but we still stop BLE once streaming for power).
5. **Platform.** Server re-verifies every signature against the SE's attested pubkey, **recomputes**
   coherence from the actual frames (`capture-coherence.ts`), and reads `caseIntact`/`modalityCount`
   from trusted channels. The device never scores itself — the manifest is evidence, not authority.

The two orthogonal axes INTEGRITY.md §3 scores map onto hardware cleanly: **axis (a) key custody** =
the secure element (§3); **axis (b) physical-capture hardness** = the sensor stack (§4) + the tamper
subsystem (§5) + the liveness challenge (a platform-chosen nonce rendered in-scene, verified in RGB).

---

## 2. MCU choice — ESP32-S3

**Recommendation: ESP32-S3 (ESP32-S3-WROOM-1-N16R8), not the classic AI-Thinker ESP32.** The stock
board's ESP32-D0WD is adequate for one RGB stream; the verified SKU adds a second I2C bus, three
corroborating sensors, an SE on a third bus, a tamper GPIO, and a per-frame Ed25519 signing load. The
S3 wins on five concrete axes:

| Requirement                                     | Classic ESP32 (AI-Thinker)                        | ESP32-S3-WROOM-1-N16R8                               | Why it matters here                                                                              |
| ----------------------------------------------- | ------------------------------------------------- | ---------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| GPIO for camera + 2× I2C + SE + tamper + 2× ADC | ~tight; camera DVP eats most exposed pins         | 45 GPIO, flexible IO-MUX                             | Camera DVP (14) + I2C-A (2) + I2C-B (2) + tamper (1) + SE ENA (1) + EMI ADC (2) fits with margin |
| PSRAM                                           | 4 MB (QSPI), shared                               | 8 MB **octal** PSRAM                                 | JPEG framebuffer(s) + TLS + depth/thermal/EMI sensor buffers + FFT scratch                       |
| Flash                                           | 4 MB (one/limited OTA)                            | 16 MB                                                | Dual ~1.9 MB OTA app slots (re-flashability, §9) **plus** sensor driver code + certs             |
| Crypto throughput                               | SW SHA/AES only                                   | HW SHA-256 + AES + RNG accelerators                  | Per-frame `sha256` at 30 fps without starving capture                                            |
| Radio                                           | Wi-Fi/BLE share one radio (coexistence throttles) | Wi-Fi + BLE **coexistence improved**; USB-OTG native | BLE provisioning + Wi-Fi streaming without the classic modem-sleep penalty; native USB flashing  |

**Dual-core split.** Core0 owns the camera driver + JPEG + WebSocket TX (the existing hot path).
Core1 owns the sensor reads, the Ed25519 chain, and manifest signing, so signing latency never stalls
the pixel DMA. The `esp32-camera` driver on the classic ESP32 owns I2S0 for pixel DMA (the reason the
INMP441 mic can't be auto-probed, per `config.h`); the S3 uses the dedicated LCD_CAM peripheral for the
DVP camera, freeing I2S for other use and removing that landmine.

**Flash / PSRAM budget (16 MB flash / 8 MB PSRAM):**

```
FLASH (16 MB)                           PSRAM (8 MB)
  nvs / otadata / phy       ~0.1 MB       RGB JPEG framebuffer(s)   ~2.0 MB (2592×1944 fb_count=2)
  app slot 0 (ota_0)         1.9 MB       TLS/WSS buffers            ~0.1 MB
  app slot 1 (ota_1)         1.9 MB       depth 8×8 ×N history       <0.01 MB
  sensor drivers (in app)   (within app)  thermal 32×24 ×N + calib   ~0.1 MB
  attestation cert (nvs/R2)  ~1 KB        EMI ADC-DMA window + FFT   ~0.05 MB
  headroom / future OTA      ~10 MB       headroom                   ~5.6 MB
```

The image (BLE + camera + TLS ≈ 1.3 MB base, + sensor drivers + SE stack ≈ +0.4 MB) fits one ~1.9 MB
OTA slot with room to grow; two slots preserve rollback. **No Secure Boot / Flash Encryption eFuse
burns** — the board stays USB-re-flashable forever (CAMERAS.md hard requirement); key confidentiality
comes from the SE, not from locking the SoC (§9).

> **Pin-map alignment.** `src/config.h` gains a verified-SKU pin block conditioned on
> `CAM_BOARD_ESP32S3`: shared sensor I2C (`CAGI_VERIFIED_I2C_SDA/SCL`), the SE bus/address/ENA, LiDAR
> LPn+INT, and the EMI ADC pins. **`CAGI_TAMPER_GPIO` had to move on S3**: the previous default `12`
> collides with the S3 camera data line `Y7_GPIO_NUM=12`, so on `CAM_BOARD_ESP32S3` the tamper loop is
> on a free pin (`GPIO7`). See §4/§5 for the full map.

---

## 3. The secure element — the critical constraint

### 3.1 Why ATECC608 does NOT work as a drop-in

The Microchip **ATECC608A/B** is the reflexive choice (cheap, I2C, in every "add a secure element to
your ESP32" tutorial, and named in the current firmware comments). It performs **ECDSA/ECDH on NIST
P-256 only**. It has **no Ed25519 / EdDSA support** and cannot be made to produce one. The platform's
`verifyFrameChain` / `verifyManifest` verify **Ed25519** signatures. So an ATECC608 physically cannot
sign anything the platform will accept as a valid frame/manifest signature.

Two ways out:

- **(a) — RECOMMENDED — pick an Ed25519-capable secure element** and keep the platform's crypto
  unchanged. Zero fragmentation; every verified device speaks the one signature scheme the whole
  integrity core already verifies.
- **(b) — keep a P-256-only SE (ATECC608) and add a P-256 signing path.** This means the firmware
  signs the same domain-separated pre-images with ECDSA-P256, AND the platform grows a _second_
  verification path (`verifyFrameChainP256` / a P-256 manifest verifier) plus a per-device
  `sigAlg ∈ {ed25519, p256}` field in `provisioned_devices` and every consumer. That is a **real
  fragmentation cost** — two signature suites through the evaluator, the anchor rail, and every
  third-party verifier — bought only to save ~$0.50/unit on the SE. **Rejected** for the reference
  design; documented so the tradeoff is explicit if volume economics ever force it.

### 3.2 Recommended part: NXP EdgeLock **SE050** (Ed25519)

**Recommendation: NXP SE050 family (SE050C2 / SE051), I2C.** Rationale:

- **Native Ed25519 (EdDSA) + Curve25519 (X25519)** in addition to NIST ECC/RSA — it signs exactly the
  scheme the platform verifies. This is the decisive property.
- **Keys generated inside; never leave silicon.** An Ed25519 keypair is generated in the SE; the
  private half is non-extractable by construction. The MCU sends the 32/64-byte message digest/message
  over the I2C APDU channel and gets back the 64-byte signature. Full firmware compromise cannot
  exfiltrate the key — the honest basis for the `secure_element` tier (INTEGRITY.md §2).
- **Factory attestation.** The SE ships with an NXP-issued attestation keypair + certificate chaining
  to an NXP root, and supports _attested_ key generation: a signed statement that "this public key was
  generated inside this genuine SE." That statement is what we read out and bind as `attestCert` (§9),
  closing the gap where a cloned board just _claims_ `secureElement: true`.
- **I2C, 1.62–3.6 V, ~10×10 or smaller.** Drops onto the shared/secure I2C bus. Common Criteria
  EAL6+ / hardware-attack-resistant package.
- **Cost** ~$1.00–2.00 @ volume — the ATECC608 saving (b) would chase is not worth the fork.

**Evaluated alternatives (all Ed25519-capable — any is a valid second source):**

| Part                                       | Ed25519?                                                           | Bus       | Attestation                 | Notes                                                                    |
| ------------------------------------------ | ------------------------------------------------------------------ | --------- | --------------------------- | ------------------------------------------------------------------------ |
| **NXP SE050 / SE051**                      | **Yes (EdDSA)**                                                    | I2C       | NXP-signed, attested keygen | **Recommended.** Best-documented Ed25519 SE path; IoT applet.            |
| Microchip **TA100 / TA101** (Trust Anchor) | Yes (EdDSA Ed25519)                                                | I2C / SPI | Microchip provisioning      | Good 2nd source; heavier than a Trust element.                           |
| Infineon **OPTIGA Trust M** (v3+)          | Ed25519 on recent silicon; **verify part rev**                     | I2C       | Infineon cert               | Ubiquitous; confirm the specific SKU supports Ed25519 before committing. |
| **TPM 2.0** (e.g. Infineon SLB9673 I2C)    | Only if the TPM advertises `TPM_ECC_ED25519` (rev 1.59+, optional) | I2C/SPI   | EK cert (vendor root)       | Heavier stack; use on richer hosts (§12), not the ~$1 camera.            |
| Microchip **ATECC608A/B**                  | **No — P-256 only**                                                | I2C       | Yes (P-256)                 | Only under path (b); forces a platform P-256 verifier. Not recommended.  |

### 3.3 SE integration on the board

- **Bus.** SE050 on a dedicated **I2C-B** (its own bus, distinct from the sensor I2C-A) so a probe on
  the sensor bus never sees SE traffic, and so the SE can run under the tamper mesh in a protected zone
  (§5, §7). Address `0x48` (SE050 default) — `config.h` `CAGI_SE_I2C_ADDR` updated from the ATECC608
  `0x60`. `ENA` (enable) tied to a GPIO (`CAGI_SE_ENA_GPIO`) or pulled high; a reset line optional.
- **Key generation.** At factory bring-up (§9) the firmware issues the SE keygen for an Ed25519 key in
  a fixed key-id/slot, reads back the **public key** (32 bytes → 64 hex, the `signer` field) and the
  **attestation object** (the SE-signed statement + the SE's own cert chain).
- **Signing path.** `Crypto::ed25519SignHex(msg, len, seed32)` on the SE build **ignores `seed32`** and
  routes the message to the SE (APDU: EdDSA sign with the slot's key), returning the 64-byte signature
  as 128 hex chars — byte-identical to what `@noble/ed25519` produces over the same message, because
  the pre-image (`utf8(message)`) is identical on both sides (`crypto.h` already documents this
  equivalence). No change to the pre-image; only the _custody_ of the key moves from NVS into the SE.
- **Never-leaves-silicon, stated honestly.** The SE proves _this chip signed these bytes_. It does
  **not** prove the scene was real — that is axis (b), §4/§5. The SE also has **no external active-mesh
  tamper input** at this price point (§5.3): "the key never leaves silicon" is enforced against
  _firmware_ extraction, and the _tamper factor_ (an opened case → `caseIntact=false`) is what prices
  the residual "someone with physical possession probed the bus" risk.

---

## 4. Sensor subsystems

Every extra modality is chosen for one job: leave a signature that a _flat re-presentation_ of a scene
(screen/print) cannot match while a _real 3D scene_ does — and to make those signatures **agree**
across channels (cross-channel edge/occlusion alignment). Each is streamed raw; the platform scores
(`capture-coherence.ts`), the device only declares presence in the manifest.

### 4.1 RGB camera — OV5640 (or OV3660)

- **Part:** OmniVision **OV5640** (5 MP, autofocus option) or **OV3660** (3 MP) — DVP 8-bit parallel +
  SCCB (I2C-like) control, XCLK 20 MHz (the OV3660/OV5640 need 20 MHz; at 16 MHz they init over SCCB
  but never emit a frame — the exact OV3660 gotcha in `config.h`).
- **Bus/pins:** parallel DVP `Y2..Y9`, `PCLK`, `VSYNC`, `HREF`, `XCLK`, `SIOD/SIOC` — the
  `CAM_BOARD_ESP32S3` pin map in `config.h`. Streamed as JPEG, 30 fps target.
- **Coherence role:** the _reference_ channel. Its edges/occlusion boundaries must align with depth
  discontinuities (LiDAR) and thermal boundaries (IR). A photo-of-a-scene has RGB edges with **no**
  matching depth step — the cross-channel disagreement is the tell.
- **Liveness carrier:** the platform's challenge nonce (a rendered token/QR the adjudicator chooses) is
  read back out of the RGB frame — `livenessVerified` (part of the fused-ceiling conjunction).

### 4.2 LiDAR / ToF depth — VL53L5CX (8×8 multizone)

- **Part:** ST **VL53L5CX** — 8×8 multizone time-of-flight, I2C, up to ~15 Hz, ~4 m range. Firmware
  `DepthFrame` is already 8×8 (`sensors/lidar.h`).
- **Bus/pins:** **I2C-A** (shared sensor bus) at up to 1 MHz; plus `LPn` (shutdown/address-select) and
  `INT` (data-ready) GPIOs → `CAGI_LIDAR_LPN_GPIO` / `CAGI_LIDAR_INT_GPIO`. Default I2C address `0x29`.
  Needs a firmware-side `Wire` bring-up + `vl53l5cx_set_resolution(64)` (the `lidar.cpp` TODO).
- **Physical feature → coherence:** **depth variance / flatness.** A real scene fills the 8×8 with a
  distribution of distances; a screen or held photo returns a **near-constant plane** with sharp
  rectangular borders → high depth-flatness → replay-like. Do **not** smooth/normalise on-device — the
  platform reads the real spatial variance.
- **Spoof it defeats:** **screen replay + printed photo.** A flat emitter/paper cannot fake volumetric
  depth aligned to the RGB occlusion edges.

### 4.3 Thermal / long-wave IR — MLX90640 (32×24)

- **Part:** Melexis **MLX90640** — 32×24 far-IR array, I2C up to 1 MHz, ~1–16 Hz refresh. Firmware
  `ThermalFrame` is 32×24 in centi-°C (`sensors/thermal.h`).
- **Bus/pins:** **I2C-A** (shared), address `0x33`. Bring-up: `Wire.setClock(1000000)`,
  `MLX90640_SetRefreshRate(0x05 /*16 Hz*/)`, `DumpEE`/`ExtractParameters`, then
  `GetFrameData`/`CalculateTo` → per-pixel °C (the `thermal.cpp` TODO).
- **Physical feature → coherence:** **thermal distribution vs uniformity.** A real scene has warm
  bodies/cool background (a _distribution_); an emissive display or a print reads as a **near-uniform
  panel/paper temperature**, often a perfect warm rectangle → high thermal-uniformity → replay-like.
- **Spoof it defeats:** **display replay** especially — an LCD/OLED's whole face sits near one
  temperature regardless of the "scene" it shows.
- **Cost note:** the MLX90640 (~$25) is the single most expensive sensor and the main gap between the
  cheap and full BOM (§8). A lower-res Panasonic Grid-EYE AMG8833 (8×8, ~$15) is a downgrade option.

### 4.4 EMI / flicker front-end — display-refresh + switching-noise detector

No single COTS part; this is a **designed** front-end. Goal: detect that an **emissive display** is in
frame via (a) its near-field electromagnetic refresh/pixel-clock signature and (b) its optical flicker.

Two complementary sensors, both folded into one EMI/`emi` modality:

1. **E-field / near-field EM probe → envelope detector → ADC.** A short PCB whip or a small loop
   antenna (a few mm trace loop) picks up the display's near-field emission (refresh 50–240 Hz +
   line/pixel-clock kHz/MHz harmonics). Feed it through a passive envelope detector (Schottky diode +
   RC, e.g. BAT54 + 100 pF/100 kΩ) into **ADC1** (`CAGI_EMI_ADC_PIN`), sampled by an ADC-DMA window,
   FFT'd on the S3 (arduinoFFT). Energy concentrated at display-refresh harmonics ⇒ "a screen is
   present."
2. **Fast photodiode flicker sensor.** A photodiode (e.g. BPW34) + transimpedance amp (one op-amp,
   e.g. MCP6001) into a second ADC1 channel (`CAGI_EMI_FLICKER_ADC_PIN`), sampled 50–1000 Hz. Backlight
   PWM (~200 Hz–2 kHz) and frame refresh (50/60/120 Hz) show up as strong narrowband flicker peaks;
   sunlight/incandescent are flat/broadband. FFT on-device, forward the coarse `EmiSpectrum` (64 bins,
   `sensors/emi.h`) — preserve the peaks (they _are_ the signal).

- **Bus/pins:** ADC1 channels only (ADC2 is unusable while Wi-Fi is active on ESP32). Reserve two
  ADC1-capable S3 GPIOs (`GPIO1`, `GPIO4`) → `CAGI_EMI_ADC_PIN` / `CAGI_EMI_FLICKER_ADC_PIN`.
- **Physical feature → coherence:** **`emiDisplaySignature`** — refresh-rate harmonics in EM and/or
  narrowband PWM/refresh flicker in the optical channel.
- **Spoof it defeats:** **screen replay** (the EM/flicker tell survives even when depth/thermal are
  spoofed by an elaborate rig).
- **Placement & isolation (the hard part of this sensor):** the board's _own_ switchers (the 3V3 buck)
  and the camera pixel clock emit exactly the kind of noise the probe hunts for. Mitigations:
  - Put the antenna/probe at the board **edge**, oriented toward the lens field-of-view, with a ground
    pour/guard between it and the buck inductor and the camera clock traces.
  - Use a **synchronous** switcher at a _fixed, known_ frequency (e.g. 1.5 MHz) so its harmonics land in
    known FFT bins that firmware can mask/null out (a self-EMI subtraction against a factory baseline).
  - Prefer the **optical flicker** channel as primary (it is immune to the board's own EM), and use the
    E-field probe as corroboration. The optical path can't be nulled by the board's switching noise.
  - Capture a **per-unit self-EMI baseline** at factory (nothing in frame) and subtract it at runtime.

### 4.5 Sensor bus / pin summary (S3 verified build)

| Signal            | Net         | S3 GPIO             | config.h macro             | Notes                          |
| ----------------- | ----------- | ------------------- | -------------------------- | ------------------------------ |
| Sensor I2C data   | SDA-A       | 8                   | `CAGI_VERIFIED_I2C_SDA`    | VL53L5CX + MLX90640 shared     |
| Sensor I2C clock  | SCL-A       | 9                   | `CAGI_VERIFIED_I2C_SCL`    | up to 1 MHz                    |
| LiDAR shutdown    | LPn         | 5                   | `CAGI_LIDAR_LPN_GPIO`      | VL53L5CX address-select/enable |
| LiDAR data-ready  | INT         | 6                   | `CAGI_LIDAR_INT_GPIO`      | optional (poll fallback)       |
| EMI E-field ADC   | ADC1_CH0    | 1                   | `CAGI_EMI_ADC_PIN`         | envelope-detected near-field   |
| EMI flicker ADC   | ADC1_CH3    | 4                   | `CAGI_EMI_FLICKER_ADC_PIN` | photodiode TIA                 |
| SE I2C data/clock | SDA-B/SCL-B | 43/44 (or shared A) | `CAGI_SE_SDA/SCL_GPIO`     | SE050, addr `0x48`             |
| SE enable         | ENA         | pull-up (opt. GPIO) | `CAGI_SE_ENA_GPIO`         | tie high or GPIO-gate          |
| Tamper loop       | TMPR        | 7                   | `CAGI_TAMPER_GPIO` (S3)    | **moved off 12** (=Y7 on S3)   |

> Thermal MLX90640 and LiDAR VL53L5CX share I2C-A (distinct addresses `0x33` / `0x29`). If bus loading
> or the 1 MHz thermal clock disturbs ToF timing during bring-up, split them onto I2C-A and a
> bit-banged/secondary bus; the HAL is bus-agnostic.

---

## 5. Tamper subsystem — case-open detection

**This is the honest section.** The bar (INTEGRITY.md §3.2) is _tamper-EVIDENT_: an opening is
reliably **detected and recorded**, which drives `caseIntact=false` and collapses the device-tier
strength (`TAMPER_DISCOUNT = 0.02`). _Tamper-PROOF/RESPONSIVE_ — actively zeroizing keys on intrusion —
needs HSM-grade hardware a ~$1 SE does not carry. We ship the evident tier and are explicit about the
gap.

### 5.1 Intrusion sensing

- **Primary: normally-closed (NC) loop / conductive mesh.** A loop runs across the enclosure seam (a
  pogo/spring contact bridged only when the lid is screwed shut) or, better, a **serpentine conductive
  trace on a flex** laminated to the inside of the lid. Read on `CAGI_TAMPER_GPIO` with
  `INPUT_PULLUP`: **case shut = loop closed = pin LOW; case opened / mesh cut = pin floats HIGH.** The
  first confirmed HIGH latches (`tamper.cpp` already implements the latch/poll/debounce).
- **Optional second sensor:** a surface-mount **light sensor** (phototransistor) inside an opaque case
  — any interior light = case opened — as an independent corroborating trip. And/or a **micro-switch**
  (SPST-NC) under the lid for a mechanical trip. Two independent triggers OR'd raise the cost of a
  clean bypass.
- **Mesh vs micro-switch tradeoff:** a single micro-switch is trivially defeated (drill a hole away
  from the switch, or hold the plunger). A **full conductive mesh** covering the lid + walls forces the
  attacker to cut a trace to reach the PCB, tripping the loop. A mesh over the **die/SE zone**
  specifically (a fine serpentine on an inner layer under the SE, §7) is the cheap approximation of an
  HSM envelope — it makes _probing the SE bus_ trip the latch.

### 5.2 Latch that survives power loss

An attacker will cut power before opening the case, so the latch must live in an **always-on domain**:

- **RTC backup domain + coin cell.** The tamper GPIO and a tiny always-on comparator/latch sit on the
  ESP32-S3 **RTC domain**, backed by a **CR2032** on `VBAT_RTC`. An opening while the main rail is off
  still floats the loop; an RTC-domain wake/latch (or an external SR latch — a `74LVC1G74` set by the
  loop edge, cleared only by an SE-gated line) records it. On next boot the firmware reads the latched
  state from the RTC/latch **and** the NVS mirror.
- **NVS mirror (fast-read, best-effort).** `tamper.cpp` writes `CAGI_TAMPER_NVS_KEY` in the `cagi`
  namespace. Honest caveat already in the firmware: a full NVS wipe/reflash can clear the NVS copy — so
  the NVS latch alone is best-effort. The **authoritative** latch on the responsive tier is the
  hardware SR latch / SE (below), which a reflash cannot clear.

### 5.3 Tamper _response_ — what the tiers actually can and cannot do

| Tier                               | Mechanism                                                                                                                                                    | On breach                                                                                                               | Honest limit                                                                                                                                                                                                                  |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Evident (shipped)**              | NC loop/mesh → GPIO → NVS + RTC-domain latch; SE-signed manifest carries `caseIntact`                                                                        | `caseIntact=false` **latched**; every subsequent manifest reports it; platform applies `TAMPER_DISCOUNT` → source ~dead | Key is **not** erased. A determined attacker with possession can still probe the (now known-untrusted) bus. Detection ≠ prevention.                                                                                           |
| **Responsive (best-effort, +BOM)** | Breach edge triggers an SR latch on the coin-cell domain; MCU on next power reads it and issues an SE **delete-key / lock-slot APDU**; mesh over the SE zone | Same as above **plus** the firmware attempts to render the SE key unusable                                              | Defeated by cutting power _and_ never re-powering under firmware control (attacker reads the die offline). The SE has **no autonomous mesh input** at this price — the zeroize is firmware-mediated, not hardware-autonomous. |
| **Tamper-PROOF (NOT this board)**  | Powered active-mesh envelope + secure supervisor that **autonomously** zeroizes on breach, independent of firmware                                           | Key gone before probing is possible                                                                                     | Needs an HSM-grade part + battery + potted envelope; $$$, out of scope for a ~$1 SE camera (INTEGRITY.md §3.2 explicitly does **not** ask for this).                                                                          |

The continuous score wants exactly the evident version: the tamper _factor lowers_ strength, it is not
a binary kill-switch, and a lowered-but-nonzero source composed against corroborating sources is more
honest than a hard reject.

### 5.4 Physical hardening

- **Conformal coat / pot the SE + tamper zone.** A conformal coating (acrylic/urethane) over the SE
  and its bus, or full **epoxy potting** of the SE + tamper-mesh sub-region, so a probe must chip
  through potting (visible, slow) and likely cut the mesh (trips the latch). Do **not** pot the whole
  board (thermal, rework, the camera/optics) — pot the security zone only.
- **Security screws** (pentalobe/tri-wing/one-way) on the enclosure so a casual open needs a special
  bit — friction, not a defense, but raises the "evident" bar and pairs with the mesh.
- **Tamper-evident label** across the seam over a screw (a void-if-removed sticker) as a cheap visual
  layer.
- **Mesh-vs-die attack tradeoff:** a lid-only mesh stops lid removal but not side-drilling; a
  full-envelope mesh (lid+walls) stops drilling but costs a flex + assembly. For this SKU, spec a
  **lid mesh + a local SE-zone mesh on an inner PCB layer** (§7) — the SE-zone mesh is the part that
  actually protects the bus, the lid mesh catches the common "unscrew and probe" attack.

---

## 6. Power

### 6.1 Topology

```
USB-C (VBUS 5V, CC-config; PD optional for margin)
   ├─▶ input protection (ideal-diode / TVS, e.g. SP0503 + P-FET reverse block)
   ├─▶ Buck 5V→3V3 (synchronous, FIXED 1.5 MHz — e.g. TPS62840/MP2161)  ──▶ 3V3 rail (SoC, sensors, SE)
   │       └─(3V3)─▶ optional LDO 3V3→1V8 for SE050 VDD(IO) if 1V8 chosen
   ├─▶ [optional Li-ion] charger (MCP73831/BQ24074) ──▶ cell ──▶ PPM/boost ──▶ 3V3 path
   └─▶ CR2032 coin cell ──▶ VBAT_RTC (RTC domain + tamper latch)  [ALWAYS-ON, µA-class]
```

- **3V3 synchronous buck at a fixed, known frequency** so its harmonics land in predictable FFT bins
  the EMI front-end can null (§4.4). Avoid a spread-spectrum/PFM-hopping regulator here — a _fixed_
  tone is easier to subtract than a smeared one.
- **SE050** runs at 3V3 (or 1V8 via a small LDO if the chosen SKU is 1V8-IO); low current.
- **Optional Li-ion** for a cordless "security-cam" placement; the base SKU is USB-C-powered.

### 6.2 Power budget

| Subsystem                       | Active (mA @3V3)          | Idle/sleep (mA)           | Notes                          |
| ------------------------------- | ------------------------- | ------------------------- | ------------------------------ |
| ESP32-S3 core (compute)         | 40–80                     | 0.8 (light sleep)         | dual-core, signing bursts      |
| Wi-Fi TX (streaming)            | 180–240 (peak ~500 burst) | — (radio off when paused) | dominant load                  |
| OV5640 RGB + XCLK               | 110–140                   | 1 (standby)               | streaming                      |
| VL53L5CX ToF                    | 20 (ranging)              | 0.005 (LPn low)           | 15 Hz                          |
| MLX90640 thermal                | 20–25                     | 0.003 (standby)           | 16 Hz                          |
| EMI front-end (probe+TIA+diode) | 2–5                       | <0.5                      | op-amp + ADC sampling          |
| SE050                           | 5–30 (during sign APDU)   | 0.05 (idle)               | brief per-frame/manifest       |
| Tamper loop + RTC latch         | —                         | **0.005–0.02**            | **always-on (coin cell)**      |
| **Total (streaming, active)**   | **~380–540**              |                           | fits USB-C 5V easily; ~2–2.7 W |
| **Total (paused, radio off)**   | ~60–90                    |                           | control-poll cadence           |

**Sequencing.** Bring up 3V3 → sensors + SE (I2C stable) → camera XCLK last (the OV-series wants a
clean XCLK after its rails settle). The RTC/tamper domain is **always powered first** from the coin
cell and is never gated — the latch must be live before, during, and after every main-rail transition,
so an open during shipping/power-off is still caught.

---

## 7. PCB

### 7.1 Stack-up

**4-layer** (the EMI/tamper isolation and the camera DVP routing justify it over 2-layer):

```
L1  Signal  — camera DVP, sensor I2C, connectors, RF antenna keepout
L2  GND     — solid reference plane (unbroken under the camera clock + EMI probe)
L3  PWR     — 3V3 / 1V8 pours; the SE-zone tamper mesh serpentine occupies a carved inner region here
L4  Signal  — SE I2C-B (buried under the mesh), tamper loop, slow signals
```

### 7.2 Placement / partitioning

- **Three zones, physically separated:**
  1. **Compute/RF zone:** ESP32-S3 module + antenna at a board edge with the mandated **antenna
     keepout** (no copper under the WROOM-1 antenna; keep the buck + camera clock away from it).
  2. **Sensor zone:** RGB camera (its own connector/optics), LiDAR + thermal on I2C-A along one edge,
     apertures aligned (§10).
  3. **Security zone (protected):** SE050 + its I2C-B, the tamper latch, and the tamper-mesh region —
     grouped tightly, buried on L4 under the L3 mesh serpentine, ready for local potting (§5.4).
- **EMI probe isolation:** the E-field probe/antenna sits at the _opposite_ edge from the buck
  inductor and the camera pixel-clock traces, with L2 ground guarding. The photodiode faces the lens
  FoV. Route the buck's switch node short and shielded; keep the fixed 1.5 MHz switcher's loop tiny.
- **Camera clock discipline:** XCLK/PCLK are the loudest on-board aggressors for the EMI channel —
  route them over an unbroken L2 plane, guard-trace them, and keep them off the security/EMI zones.

### 7.3 Connectors, test points, DFM

- **Connectors:** USB-C (power + native S3 USB for flashing/console), the camera FPC connector, a
  0.1" (or Tag-Connect) **UART/JTAG debug header** for bring-up, the tamper-loop 2-pin header to the
  lid mesh, optional Li-ion JST.
- **Test points:** 3V3, 1V8, GND, SDA-A/SCL-A, SDA-B/SCL-B, `TMPR`, `SE_ENA`, `EMI_ADC`,
  `EMI_FLICKER`, XCLK — for board bring-up and factory ICT.
- **DFM notes:** keep the security zone reworkable pre-pot (do all rework before potting); Tag-Connect
  footprint avoids a populated header inside a potted zone; single-side-heavy placement eases assembly;
  the mesh flex mates via a board-edge connector so the lid is a separable sub-assembly. Leave the
  eFuse **unburned** — nothing on the board or in the pick-and-place programs Secure Boot/Flash
  Encryption (§9).

---

## 8. Full BOM + per-unit cost

Rough unit costs at low-mid volume (hundreds–thousands); indicative, not quotes.

| Ref | Part                                                             | Qty | Function                                            | ~Unit cost | Notes                           |
| --- | ---------------------------------------------------------------- | --- | --------------------------------------------------- | ---------- | ------------------------------- |
| U1  | ESP32-S3-WROOM-1-N16R8                                           | 1   | MCU + Wi-Fi/BLE, 16MB flash / 8MB PSRAM             | $3.50      | compute+RF domain               |
| U2  | OV5640 (or OV3660) + lens                                        | 1   | RGB capture (`rgb`)                                 | $4.00      | DVP+SCCB; 20 MHz XCLK           |
| U3  | **NXP SE050C2** (Ed25519 SE)                                     | 1   | key in silicon, attestation cert (`secure_element`) | $1.50      | **Ed25519 — the §3 constraint** |
| U4  | VL53L5CX (8×8 ToF)                                               | 1   | depth (`lidar`) — anti-replay flatness              | $6.00      | I2C-A                           |
| U5  | MLX90640 (32×24 IR)                                              | 1   | thermal (`ir`) — panel-uniformity tell              | $25.00     | I2C-A; the cost driver          |
| U6  | EMI front-end (BAT54 + BPW34 + MCP6001 + passives + PCB antenna) | 1   | display EMI/flicker (`emi`)                         | $1.50      | designed, not COTS              |
| S1  | Chassis tamper: NC switch + lid conductive-mesh flex             | 1   | case-open (`caseIntact`)                            | $1.00      | evident tier                    |
| U7  | 74LVC1G74 SR latch + CR2032 holder + cell                        | 1   | always-on tamper latch                              | $0.60      | RTC-domain backup               |
| PM1 | 3V3 sync buck (fixed 1.5 MHz) + inductor/caps                    | 1   | 3V3 rail                                            | $0.80      | EMI-predictable                 |
| PM2 | USB-C recept + CC + TVS/ESD                                      | 1   | power/data in                                       | $0.60      | native USB flashing             |
| PCB | 4-layer PCB + assembly                                           | 1   | board                                               | $2.50      | 3-zone layout                   |
| ENC | Enclosure (screws, apertures, mesh lam.)                         | 1   | mechanical                                          | $2.00      | security screws + windows       |
| —   | Conformal coat / SE-zone pot                                     | 1   | hardening                                           | $0.50      | security zone only              |

**Roll-up:**

| Tier                                   | Included                                                                            | Approx. per-unit |
| -------------------------------------- | ----------------------------------------------------------------------------------- | ---------------- |
| **Evident (recommended verified SKU)** | U1–U7, S1, PM1/2, PCB, ENC, coat — with **MLX90640**                                | **≈ $49–55**     |
| **Evident, cost-reduced**              | same but **OV3660** + **AMG8833 (8×8 thermal, ~$15)** instead of MLX90640           | **≈ $38–42**     |
| **Responsive (best-effort tamper)**    | Evident + full lid+SE-zone active mesh, secure supervisor, SE zeroize path, potting | **≈ $65–80**     |
| **Tamper-PROOF (HSM active envelope)** | not this board — dedicated secure MCU + powered mesh + battery                      | **$150+**        |

The MLX90640 dominates; ~half the "evident" BOM is the thermal channel. If a program is cost-sensitive
and can accept a weaker thermal tell, drop to the 8×8 AMG8833 or omit `ir` entirely (a 2-modality
`rgb+lidar+emi` unit still qualifies for the fused ceiling: `modalityCount ≥ 3` is met by
rgb+lidar+emi).

---

## 9. Manufacturing, provisioning & attestation flow

The goal: bind `hwid` + the SE's attested **Ed25519 public key** + the SE **attestation certificate**
into `provisioned_devices`, seal the tamper mesh, and keep the board **re-flashable** (no eFuse burns).

**Factory flow (per unit):**

1. **Flash the verified firmware** over native USB (`pio run -e esp32cam-verified -t upload`). No Secure
   Boot / Flash Encryption / download-disable eFuse is ever burned — CAMERAS.md hard requirement; the
   SE, not a locked SoC, holds the key.
2. **SE key generation (inside the SE).** Firmware commands the SE050 to generate an **Ed25519** keypair
   in a fixed slot. The private half never exists outside the SE. Read back:
   - the **32-byte public key** → 64 hex (`signer`),
   - the SE's **attestation object** (SE-signed "this pubkey was generated inside this genuine SE") +
     the SE's cert chain to the NXP root → this is `attestCert` (`DeviceInfo.attestCert`).
3. **Self-EMI baseline + sensor self-test.** Capture a no-scene EMI baseline (for runtime self-EMI
   subtraction, §4.4) and confirm each sensor answers on its bus (VL53L5CX `0x29`, MLX90640 `0x33`,
   SE050 `0x48`). Store the baseline in NVS.
4. **Seal the security zone.** Screw the lid (tamper mesh mates), confirm the tamper loop reads LOW
   (case shut, `caseIntact=true`), then conformal-coat/pot the SE zone. From here an opening latches.
5. **Provision the device record.** Over the existing BLE flow (`deviceProvisioning.ts`): the app reads
   INFO — now advertising `sensors:["rgb","lidar","ir","emi"]`, `tamper:true`, `secureElement:true`,
   `attestCert:"…"` — mints the per-device `cagi_` key, and PIN-seals the PROVISION write. The platform
   persists `hwid` (eFuse MAC), the SE **public key** (`provisioned_devices.device_pubkey`), and the
   `attestCert` + the capability flags into the device record. **No signing seed is minted or sent** on
   the SE path (unlike the software `device_key` tier, where `devicePrivKeyHex` travels in the sealed
   blob) — the key already lives in the SE, so `devicePrivKeyHex` is **absent**.
6. **Platform attestation verify.** Server verifies `attestCert` against the SE vendor (NXP) root,
   confirming the advertised `signer` pubkey genuinely lives in a genuine SE on _this_ unit — closing
   the "a clone just claims `secureElement:true`" gap. Faith in "CommandAGI as camera manufacturer" is
   then the earned, continuous parameter (INTEGRITY.md §2/§5), not a constant.

**Re-flashability preserved.** Recovery is identical to the stock board (README "re-flashable forever"):
download mode is ROM-based; tie boot-strap low, re-flash over USB. Because the key is in the SE, a
reflash does **not** compromise or reset key custody — it only replaces app code. A firmware
downgrade/rooting is still _visible_: the signed `CaptureManifest` binds `fw` + `bootState`, so a
downgraded image that still signs valid frames is caught in the manifest (INTEGRITY.md §6).

---

## 10. Enclosure & mechanical

- **Sensor apertures (co-aligned, minimal parallax):**
  - **RGB window** — clear optical window / lens barrel for the OV5640.
  - **LiDAR window** — the VL53L5CX needs an IR-transmissive cover with a **non-reflective** aperture
    and a light baffle between it and the RGB lens (ToF is sensitive to cover-glass crosstalk; follow
    ST's cover-glass guidelines — air gap + blackened aperture).
  - **Thermal window** — the MLX90640 must see the scene through an **LWIR-transmissive** window
    (silicon or HDPE/polyethylene; **ordinary glass/plastic blocks 8–14 µm IR**). This is a real
    mechanical constraint — the thermal aperture cannot share the RGB glass.
  - **EMI probe** — the PCB antenna sits behind a non-metallic wall section (metal would shield it);
    the photodiode gets a small clear aperture facing the FoV.
    All four cluster on one face so their fields of view overlap — cross-modal coherence assumes the
    channels see roughly the same scene.
- **Tamper mesh integration:** the lid carries the conductive-mesh flex; closing/screwing the lid mates
  the mesh connector and closes the NC loop. Opening the lid breaks the loop → latch.
- **Security screws** (pentalobe/tri-wing) + a void-if-removed seal over one screw.
- **Mounting:** a 1/4"-20 tripod insert or a VESA-ish keyhole; USB-C accessible without opening;
  coin-cell **not** user-replaceable (replacing it would be a case-open → tamper trip, by design).

---

## 11. Threat-model → hardware-defense → score mapping

Each spoof, the hardware that answers it, and which scoring factor it drives (`sourceStrength` in
`integrity.ts`).

| Threat / spoof                                      | Hardware defense                                                                                                            | Score factor driven                                                                | Effect                                                                                                |
| --------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| **Screen replay** (point camera at a display)       | LiDAR flatness + thermal panel-uniformity + EMI refresh/flicker signature, cross-channel edge mismatch                      | `coherence` → `coherenceFactor`; blocks `fusedQualified`                           | Multiplicative coherence drags low (any one strong tell); source falls toward `COHERENCE_FLOOR` share |
| **Printed photo** re-presentation                   | LiDAR (flat plane, no depth step at RGB edges) + thermal (uniform paper temp)                                               | `coherence` → `coherenceFactor`                                                    | Same — no volumetric/thermal structure to match RGB                                                   |
| **Sensor-bus tap** (probe I2C/DVP to inject frames) | Tamper mesh over SE zone + case loop → opening latches                                                                      | `caseIntact=false` → `tamperFactor = TAMPER_DISCOUNT (0.02)`                       | Device-tier source effectively dead                                                                   |
| **Case-open key extraction**                        | SE key never leaves silicon (firmware can't exfiltrate); tamper latch records the open; responsive tier attempts SE zeroize | `secure_element` ceiling **only while** `caseIntact`; else `tamperFactor` kills it | Custody claim voided the moment the case is opened                                                    |
| **Firmware downgrade / rooting**                    | Signed `CaptureManifest` binds `fw` + `bootState`; SE still signs but manifest exposes it                                   | `bootState`/`fw` in manifest (platform reads) → gates fused-qualification / faith  | Downgrade visible even with a live SE                                                                 |
| **EMI masking** (shield/spoof the display's EM)     | Dual EMI path — optical **flicker** survives EM shielding; E-field probe corroborates                                       | `coherence` (emi channel)                                                          | Attacker must defeat _both_ EM and optical flicker, plus depth+thermal                                |
| **Cloned board claims `secureElement:true`**        | SE factory **attestation cert** verified to NXP root at provisioning                                                        | `mechanism` tier admission (§9)                                                    | A clone without a genuine SE attestation never registers at `secure_element`                          |
| **Replayed / forked frame chain**                   | Ed25519 hash chain + seq monotonicity + stream↔chain anchoring                                                              | `chainVerified` → `chainFactor`; `anchored` → `anchorFactor`                       | Gap/fork in an anchored chain is visible; unverified chain discounts device tier                      |
| **Emulated Ed25519 in a P-256-only SE**             | Part choice: SE050 signs Ed25519 natively (§3)                                                                              | admission to `secure_element` at all                                               | ATECC608 physically can't sign what the platform verifies — design forecloses it                      |

The fused ceiling (`0.995`) is earned only by the full conjunction — `secure_element && caseIntact &&
livenessVerified && modalityCount ≥ 3 && coherence ≥ 0.9` — i.e. exactly when this hardware has
_measurably attacked_ the replay/analog-hole gap the 0.98 cap represents. It is still `< 1`: an
elaborate physical diorama built to the nonce is never fully ruled out.

---

## 12. Variants & roadmap

| Variant                               | Key custody                                | Tamper                 | Sensors          | Tier reached                        | ~Cost        |
| ------------------------------------- | ------------------------------------------ | ---------------------- | ---------------- | ----------------------------------- | ------------ |
| **Stock ESP32-CAM**                   | none / account                             | none                   | rgb              | `account` (single RGB)              | ~$6          |
| **Software-attested** (`device_key`)  | Ed25519 seed in NVS                        | optional               | rgb (+opt)       | `device_key`                        | ~$8–12       |
| **Verified SKU — evident** (this doc) | **SE050 (Ed25519)**                        | evident (mesh+latch)   | rgb+lidar+ir+emi | `secure_element` + **fused 0.995**  | ~$49–55      |
| **Verified SKU — responsive**         | SE050 + firmware zeroize                   | best-effort responsive | rgb+lidar+ir+emi | same tier, higher tamper-resistance | ~$65–80      |
| **Richer hosts** (robot/phone/PC)     | TEE / StrongBox / Secure Enclave / TPM 2.0 | platform-dependent     | host sensors     | `secure_element` (same ladder)      | host-defined |

**The ladder is about where the key lives, not the device kind** (INTEGRITY.md §3, step 3). A robot
controller's TEE, a phone's StrongBox/Secure Enclave, and a host's TPM 2.0 all slot into the same
`secure_element` class — provided the key produces **Ed25519** (or the platform grows the P-256 path of
§3.1(b); note TPM 2.0 Ed25519 support is optional/rev-dependent, so a StrongBox/Secure Enclave that
does Ed25519 is the cleaner phone/robot path). Same attestation-cert binding, same manifest, same
score.

**Roadmap / bring-up open items:**

- Wire the real sensor drivers (VL53L5CX, MLX90640, arduinoFFT) into the HAL stubs; capture the
  interventional paired real-vs-replay-vs-print corpus that ungates `computeCoherence`
  (INTEGRITY.md §6) — hardware and the corpus are the two gates before an integrity score moves money.
- Finish the SE050 Ed25519 sign + attested-keygen APDU integration in `crypto.cpp` (replace the
  NVS-seed software path on the SE build) and the attestation-cert read in the provisioning flow.
- EMI front-end board spin: validate the self-EMI subtraction against the fixed-frequency buck and the
  optical-flicker primary path on real displays.
- Responsive-tier evaluation: measure how reliably the SE zeroize-on-tamper actually fires vs a
  power-cut attacker, and price it honestly (it is best-effort, not tamper-proof).

---

**Cross-references:** ``docs/trust/INTEGRITY.md`` (platform-internal reference) ·
``docs/platform/CAMERAS.md`` (platform-internal reference) ·
``packages/domain/core/src/integrity.ts`` (platform-internal reference) ·
``packages/domain/core/src/deviceProvisioning.ts`` (platform-internal reference) ·
``packages/domain/core/src/capture-coherence.ts`` (platform-internal reference) ·
[`../README.md`](../README.md) (hardware BOM) · ``../../src/config.h`` (platform-internal reference) (pin map) ·
``../../src/crypto.h`` (platform-internal reference) (Ed25519 signing custody) ·
``../../platformio.ini`` (platform-internal reference) (`[env:esp32cam-verified]`).
</content>
</invoke>
