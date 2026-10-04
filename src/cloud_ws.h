#pragma once
#include <Arduino.h>
#include "config.h"
#include "store.h"

// High-frame-rate frame transport: a single persistent realtime WebSocket to the session DO over which
// the camera streams binary JPEG frames (MJPEG-style) — one TLS handshake instead of one HTTPS POST per
// frame, which is what unlocks ~real frame rates. The DO fans each frame out to viewers and records a
// throttled subset. Control (sensor on/off, cadence) still rides the low-rate HTTP poll in cloud.cpp.
namespace CloudWs {
// Open the runtime socket (idempotent). On connect it announces the camera channel. Needs a registered
// session/device/token in `c` (see Cloud::ensureRegistered).
void begin(const Creds& c);
// Service the socket — must be called frequently from loop() (drives rx, heartbeats, reconnect).
void loop();
// Whether the socket is currently open (announced + ready to carry frames).
bool connected();
// Send one binary JPEG frame. Returns false if the socket isn't open (caller should skip this frame).
bool sendFrame(const uint8_t* buf, size_t len);
#if CAGI_DEVICE_SEALS
// The sealed camera's two messages (src/seal_runtime.cpp). A stamped frame as an addressed `frame`
// message (data URL), and index and seal lines as a `data` message whose text is the lines, newline-
// terminated, exactly as the device sealed them. Return false when the socket is not open.
bool sendFrameJson(const char* channelId, const uint8_t* jpeg, size_t len);
bool sendLines(const char* channelId, const char* kind, const char* lines, size_t len);
#endif
// Send one JSON text message (e.g. the runtime `status`). Only the loop's task may call it.
bool sendText(const String& json);
// Close the socket (e.g. on re-provisioning / lost creds).
void stop();
}  // namespace CloudWs
