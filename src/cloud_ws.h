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
#if CAGI_VERIFIED_SKU
// Send a signed capture-manifest sidecar (a JSON TXT frame) over the same socket. Returns false if the
// socket isn't open. The manifest rides parallel to the JPEG frames (channel CAGI_MANIFEST_CHANNEL) so
// the platform can bind it to the frame-chain segment it covers. See manifest.cpp.
bool sendManifest(const String& json);
#endif
// Close the socket (e.g. on re-provisioning / lost creds).
void stop();
}  // namespace CloudWs
