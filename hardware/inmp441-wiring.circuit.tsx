/**
 * INMP441 I2S microphone ↔ AI-Thinker ESP32-CAM wiring (tscircuit).
 *
 * This is the schematic the firmware's `src/config.h` pin map encodes — the add-on mic that gives a
 * camera audio. Render it with the tscircuit CLI (see hardware/README.md):
 *
 *   npx tsci dev hardware/inmp441-wiring.circuit.tsx
 *
 * The INMP441 is wired for the LEFT channel (L/R → GND), which is what the firmware reads. GPIO13/14/15
 * are the AI-Thinker's unused HS2 SD-card pins, free when no microSD is used.
 */

export default () => (
  <board width="50mm" height="40mm" routingDisabled>
    {/* The ESP32-CAM carrier — only the pins we use are broken out here. */}
    <chip
      name="ESP32CAM"
      footprint="pinrow8"
      pinLabels={{
        pin1: "3V3",
        pin2: "GND",
        pin3: "GPIO14_BCLK",
        pin4: "GPIO15_WS",
        pin5: "GPIO13_DIN",
        pin6: "GPIO0",
        pin7: "U0T",
        pin8: "U0R",
      }}
      schX={-6}
      schY={0}
    />

    {/* INMP441 breakout — standard 6-pin module. */}
    <chip
      name="MIC"
      footprint="pinrow6"
      pinLabels={{
        pin1: "VDD",
        pin2: "GND",
        pin3: "SD", // serial data out → ESP
        pin4: "LR", // channel select (→ GND = left)
        pin5: "WS", // word select / LRCL
        pin6: "SCK", // bit clock / BCLK
      }}
      schX={6}
      schY={0}
    />

    {/* Power */}
    <trace from=".ESP32CAM .3V3" to=".MIC .VDD" />
    <trace from=".ESP32CAM .GND" to=".MIC .GND" />
    {/* L/R tied to GND selects the left channel (the one the firmware samples). */}
    <trace from=".MIC .LR" to=".ESP32CAM .GND" />

    {/* I2S: clocks out of the ESP, data back in. */}
    <trace from=".ESP32CAM .GPIO14_BCLK" to=".MIC .SCK" />
    <trace from=".ESP32CAM .GPIO15_WS" to=".MIC .WS" />
    <trace from=".ESP32CAM .GPIO13_DIN" to=".MIC .SD" />
  </board>
);
