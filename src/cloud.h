#pragma once
#include <Arduino.h>
#include "store.h"
#include "config.h"
#include "ir_policy.h"

namespace Cloud {
// Desired sensor state the server hands back to the device — which streams the operator wants ON.
// Defaults to both enabled so a server that says nothing preserves "stream everything" behavior.
struct Control {
  bool valid = false;  // true once a response actually carried a `sensors` object or `intervalMs`
  bool cam = true;
  bool mic = true;
  /** Server-desired frame cadence (ms); 0 = the response said nothing → keep the current interval. */
  unsigned long intervalMs = 0;
  /** Server-desired frame interval floor on cellular (ms); 0 = no opinion. Cellular builds only. */
  unsigned long cellularIntervalMs = 0;
#if CAGI_IR_ENABLED
  /** The operator's IR mode, `sensors.ir`. Missing or unknown = auto, so older servers keep working. */
  IrPolicy::Mode ir = IrPolicy::Mode::Auto;
#endif
};

// Join the LAN with the stored creds. Blocks up to ~20s. Returns true once connected.
bool connectWifi(const Creds& c);
bool wifiConnected();

// Ensure we have a registered world session + device. Uses the cached ids if present; otherwise
// calls POST /worlds + POST /sessions/:id/connect-device (kind="camera") with the account API key,
// persisting the result. Mutates c.sessionId/c.deviceId. Returns false on auth/API failure.
bool ensureRegistered(Creds& c);

// Why one frame/audio POST failed — lets the loop tell "retry later" (transient) apart from
// "stop and surface an error" (the session was deleted, or the API key is bad). Without this split
// every failure looked the same and the device retried silently forever — the camera sat there
// streaming-to-nowhere while the app spun on "Signing in" with no diagnosable reason.
struct PostResult {
  bool ok = false;          // 2xx
  bool gone = false;        // 404 — session/device deleted server-side → clear cache + re-register
  bool authFailed = false;  // 401/403 — bad/revoked/expired API key → re-provisioning required
  int code = 0;             // raw HTTP status, or <0 on a transport error (no response)
};

// POST one JPEG frame to the stream endpoint. Fills *res (ok/gone/authFailed/code) when non-null and
// returns res.ok. The response carries the desired sensor state, parsed into *ctl when non-null (so a
// streaming camera learns "mic was just turned on/off" without any extra request).
bool postFrame(const Creds& c, const uint8_t* buf, size_t len, unsigned long intervalMs, PostResult* res, Control* ctl = nullptr);

// POST one WAV audio clip (channel=mic, kind=audio). Same contract as postFrame.
bool postAudio(const Creds& c, const uint8_t* buf, size_t len, PostResult* res, Control* ctl = nullptr);

// Fetch the desired sensor state without streaming anything — used while idle (cam+mic both off) so a
// stopped camera can still be remotely turned back on. Fills *ctl; returns false on transport error.
bool pollControl(const Creds& c, Control* ctl);

// A recent block of the contract's chain (GET <api>/public/chain/block), as the canonical JSON a seal
// names: {"chain":"solana:devnet","hash":"…","slot":123}. False when the platform has none (503) or the
// call failed; `out` is then unchanged.
bool chainBlock(const Creds& c, String& out);
}  // namespace Cloud
