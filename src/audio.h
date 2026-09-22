#pragma once
#include <Arduino.h>

// INMP441 I2S microphone — fully optional and independent of the camera. Mirrors the Camera:: shape
// (begin/capture/release) so the main loop treats the two sensors symmetrically: either can be
// absent and the other still streams. Each capture() yields one self-contained WAV clip (header +
// PCM16) ready to POST as audio/wav.
namespace Audio {
// Install the I2S driver and probe the mic. Returns false if I2S init fails (no mic wired / bad pins)
// — the caller keeps running without audio. Safe to call once at boot.
bool begin();
// True once begin() succeeded and the mic is usable.
bool available();

// Record CAGI_AUDIO_CLIP_MS of mono audio into an internal (PSRAM) buffer and expose it as a complete
// WAV file. On success sets *buf/*len to the WAV bytes and returns true; the buffer is owned by this
// module (valid until the next capture()), so the caller must NOT free it. release() is a no-op kept
// for symmetry with Camera::release().
bool capture(uint8_t** buf, size_t* len);
void release();
}  // namespace Audio
