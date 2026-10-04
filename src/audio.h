#pragma once
#include <Arduino.h>

// Microphone (an INMP441 on I2S, or the DFR1154's on-board PDM mic) — fully optional and independent of the camera. Mirrors the Camera:: shape
// (begin/capture/release) so the main loop treats the two sensors symmetrically: either can be
// absent and the other still streams. Each capture() yields one self-contained WAV clip (header +
// PCM16) ready to POST as audio/wav.
namespace Audio {
// Install the I2S driver and probe the mic. Returns false if I2S init fails (no mic wired / bad pins)
// — the caller keeps running without audio. Safe to call once at boot.
bool begin();
// True once begin() succeeded and the mic is usable.
bool available();

// One CAGI_AUDIO_CLIP_MS clip of mono audio as a complete WAV file (in PSRAM). On success sets
// *buf/*len to the WAV bytes and returns true; the buffer is owned by this module, so the caller
// must NOT free it, and calls release() when it is done with it.
//   ESP32-S3: non-blocking. A capture task records continuously; capture() returns the newest
//             finished clip, or false at once if none is ready.
//   ESP32:    blocks for CAGI_AUDIO_CLIP_MS while it records (unchanged behaviour).
bool capture(uint8_t** buf, size_t* len);
void release();
// The operator's mic state. On the S3, false stops the I2S clock and drops an unposted clip; on the
// classic ESP32 it does nothing (that path records only inside capture()).
void setEnabled(bool on);
}  // namespace Audio
