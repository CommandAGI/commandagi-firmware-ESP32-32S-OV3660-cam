#pragma once
#include <Arduino.h>

namespace Camera {
// Initialize the OV2640 (JPEG mode). Returns false on failure (bad wiring / no PSRAM).
bool begin();
// Capture one JPEG frame. On success, sets *buf/*len to the framebuffer and returns true; the caller
// MUST call release() when done (the buffer is owned by the camera driver, not the heap).
bool capture(uint8_t** buf, size_t* len);
void release();
// Deinit + re-init the sensor — runtime recovery when capture() keeps failing.
bool reinit();
// Exhaustive hardware probe (CAGI_CAMTEST builds): SCCB chip-ID + frame grabs across XCLK/sizes.
// Prints a verdict over serial; used to tell a dead sensor from a config/power/ribbon problem.
void selftest();
}  // namespace Camera
