// CommandAGI ESP32-CAM firmware
// ──────────────────────────────
// A factory-fresh camera advertises a BLE provisioning service. The CommandAGI app discovers it,
// hands it Wi-Fi creds + a per-device account API key, and the camera then registers a machine
// session under that account and streams JPEG frames (and, if an INMP441 mic is wired, WAV audio) to
// the dashboard. Camera and mic are independent: either can be absent and the other still streams.
// The operator can remotely turn each sensor on/off; the device reconciles to that desired state. All
// state lives in NVS, so it reconnects on its own after a power cycle. Re-flashable over USB forever
// (no Secure Boot / Flash Encryption); a factory-reset only wipes the creds. See README.md and
// docs/CAMERAS.md.

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "store.h"
#include "status.h"
#include "camera.h"
#include "audio.h"
#include "cloud.h"
#include "cloud_ws.h"
#include "ble_prov.h"
#include "tamper.h"
#include "manifest.h"
#if CAGI_SENSOR_LIDAR
#include "sensors/lidar.h"
#endif
#if CAGI_SENSOR_THERMAL
#include "sensors/thermal.h"
#endif
#if CAGI_SENSOR_EMI
#include "sensors/emi.h"
#endif

namespace {
Creds g_creds;
bool g_streaming = false;
bool g_cameraOk = false;
bool g_audioOk = false;
// Operator-desired sensor state (mirrors the server). Default ON so a fresh camera streams everything
// until told otherwise. Updated from every stream-POST response and from the idle control poll.
bool g_camDesired = true;
bool g_micDesired = true;
// Live frame cadence (ms). Starts at the compile-time default but is overwritten by the server's
// desired interval (operator/app-configurable) on every stream/control response.
uint32_t g_frameIntervalMs = CAGI_FRAME_INTERVAL_MS;
uint32_t g_nextFrame = 0;
uint32_t g_nextAudio = 0;
uint32_t g_nextControl = 0;
uint32_t g_retryAt = 0;
uint32_t g_lastStreamOkMs = 0;  // last time a frame went out — gates BLE stop/revive (radio sharing)
uint32_t g_wsStartedAt = 0;     // when we opened the frame WebSocket — re-register if it won't connect
#if CAGI_VERIFIED_SKU
uint32_t g_nextManifest = 0;    // when to emit the next signed capture manifest (sidecar)
uint32_t g_manifestSeq = 0;     // capture-window sequence the manifest binds to
#endif
uint32_t g_captureFails = 0;  // consecutive Camera::capture() failures (for surfacing + sensor recovery)

void reloadCreds() { g_creds = Store::load(); }

// Fold a server-provided desired state into our flags (only when the response actually carried one).
void applyControl(const Cloud::Control& ctl) {
  if (!ctl.valid) return;
  g_camDesired = ctl.cam;
  g_micDesired = ctl.mic;
  // Adopt the server-desired frame cadence (clamped to a sane device range). 0 = no opinion.
  if (ctl.intervalMs > 0) {
    uint32_t want = ctl.intervalMs < CAGI_FRAME_INTERVAL_MIN_MS ? CAGI_FRAME_INTERVAL_MIN_MS
                    : (ctl.intervalMs > 3600000 ? 3600000 : ctl.intervalMs);
    if (want != g_frameIntervalMs) {
      Serial.printf("[cfg] frame interval %lu → %lu ms\n", (unsigned long)g_frameIntervalMs, (unsigned long)want);
      g_frameIntervalMs = want;
    }
  }
}

// Turn a failed media POST into the right recovery + a VISIBLE status, and tell the loop to bail this
// iteration. This is the fix for the "app spins on 'Signing in' forever" class of bug: previously
// every non-404 failure (bad API key, 5xx, network black-hole) was swallowed and retried silently, so
// the camera streamed-to-nowhere with no log and no status the app could show. Returns true if the
// loop should return now. Caller must already have released the capture buffer.
bool handlePostFailure(const Cloud::PostResult& r) {
  if (r.ok) return false;
  if (r.gone) {  // 404 — session/device deleted server-side: drop the cache and re-register cleanly
    Store::clearRegistration();
    g_creds.sessionId = "";
    g_creds.deviceId = "";
    g_streaming = false;
    g_retryAt = 0;
    Status::set("registering", "session removed — reconnecting");
    return true;
  }
  if (r.authFailed) {  // 401/403 — the API key itself is rejected: only re-provisioning can fix it
    g_streaming = false;
    g_retryAt = millis() + 30000;  // stop hammering; surface a clear, app-readable error
    Status::set("auth_failed", "API key rejected (" + String(r.code) + ") — re-pair in the app");
    return true;
  }
  // Transient (5xx / timeout / network hiccup): surface "offline" once on the streaming→down edge
  // (postMedia already logs every attempt's code), then retry shortly.
  if (g_streaming) Status::set("offline", "stream POST " + String(r.code));
  g_streaming = false;
  g_retryAt = millis() + 3000;
  return true;
}

// A media POST just succeeded — we're genuinely streaming. Record it, and on the first success FREE THE
// SHARED RADIO: stop BLE and turn off Wi-Fi modem-sleep. The classic ESP32 forces modem-sleep while BLE
// is active, which is what was throttling each upload to seconds; dropping BLE lets Wi-Fi run full-rate.
void noteStreamOk() {
  g_lastStreamOkMs = millis();
  if (BleProv::isUp()) {
    Serial.println("[radio] streaming — stopping BLE + disabling Wi-Fi modem-sleep for throughput");
    BleProv::stop();
    WiFi.setSleep(false);
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[boot] CommandAGI ESP32-CAM " CAGI_FW_VERSION);

  Store::begin();
  Status::set("idle");

#ifdef CAGI_CAMTEST
  // Hardware diagnostic build: probe the camera in isolation so we can tell a dead sensor from a
  // config/power/ribbon problem. With -DCAGI_CAMTEST_BLE it brings BLE up FIRST (and -DCAGI_CAMTEST_WIFI
  // brings Wi-Fi up too) before probing, to isolate whether radio/RAM contention is what starves
  // capture. Flash with PLATFORMIO_BUILD_FLAGS, read serial, then reflash the normal firmware.
#ifdef CAGI_CAMTEST_BLE
  Serial.println("[camtest] bringing BLE up first (contention test)");
  BleProv::begin();
  delay(500);
#endif
#ifdef CAGI_CAMTEST_AUDIO
  Serial.println("[camtest] calling Audio::begin() before probe (I2S contention test)");
  g_audioOk = Audio::begin();
  Serial.printf("[camtest] audio begin = %s\n", g_audioOk ? "ok" : "false");
#endif
#ifdef CAGI_CAMTEST_WIFI
  if (Store::hasCreds()) {
    Creds tc = Store::load();
    Serial.printf("[camtest] connecting Wi-Fi to '%s' (contention test)\n", tc.ssid.c_str());
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.begin(tc.ssid.c_str(), tc.psk.length() ? tc.psk.c_str() : nullptr);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
    Serial.printf("[camtest] wifi %s\n", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "FAILED");
  } else {
    Serial.println("[camtest] no stored creds — skipping Wi-Fi contention test");
  }
#endif
  Camera::selftest();
  return;
#endif

  // Bring up BLE FIRST: the NimBLE controller needs internal RAM for its mutexes/queues, and the
  // camera driver grabs a big chunk of DMA-capable internal RAM. Initializing the camera first
  // starves NimBLE (it asserts on a null mutex handle and boot-loops). BLE-first reserves its memory,
  // and wiring the STATUS notifier before the camera means a camera failure still notifies the app.
  BleProv::begin();

  g_cameraOk = Camera::begin();
  g_audioOk = Audio::begin();  // optional INMP441 — false (and harmless) if absent
  BleProv::setMicPresent(g_audioOk);

#if CAGI_VERIFIED_SKU
  // Verified-camera SKU: bring up the tamper latch, corroborating sensors, and the manifest signer.
  // All of these are no-ops on a build that didn't compile them in.
  Tamper::begin();
#if CAGI_SENSOR_LIDAR
  Sensors::Lidar::begin();
#endif
#if CAGI_SENSOR_THERMAL
  Sensors::Thermal::begin();
#endif
#if CAGI_SENSOR_EMI
  Sensors::Emi::begin();
#endif
  Manifest::begin();
#endif
  if (!g_cameraOk && !g_audioOk) Status::set("error", "no camera or mic");
  else if (!g_cameraOk) Status::set("error", "camera init failed");

  if (Store::hasCreds()) {
    reloadCreds();
    Serial.println("[boot] have stored creds — connecting");
  } else {
    Serial.println("[boot] factory-fresh — waiting for provisioning over BLE");
  }
}

void loop() {
#ifdef CAGI_CAMTEST
  delay(1000);  // diagnostic build: the probe ran once in setup(); nothing else to do
  return;
#endif
  // The app wrote fresh creds — reload and force a clean reconnect (possibly a new account).
  if (BleProv::consumeNewProvisioning()) {
    reloadCreds();
    g_streaming = false;
    g_retryAt = 0;
    CloudWs::stop();  // drop the old socket — new account/creds will re-register + reconnect
    WiFi.disconnect(true, false);
  }

  if (!Store::hasCreds()) {  // nothing to do but advertise
    delay(200);
    return;
  }

  // Radio sharing: if the stream has been down for a while AND BLE is off, re-advertise so the app can
  // re-pair a stuck camera (wrong Wi-Fi / revoked key never streams, so BLE simply stays up). Re-enable
  // modem-sleep first — BLE coexistence requires it. A healthy, streaming camera never hits this.
  if (!BleProv::isUp() && (millis() - g_lastStreamOkMs) > CAGI_BLE_REVIVE_MS) {
    Serial.println("[radio] stream down — re-advertising BLE for re-pairing");
    WiFi.setSleep(true);
    BleProv::begin();
  }

  uint32_t now = millis();
  if (now < g_retryAt) {
    delay(50);
    return;
  }

  if (!Cloud::wifiConnected()) {
    g_streaming = false;
    if (!Cloud::connectWifi(g_creds)) {
      g_retryAt = millis() + 5000;  // out of range / bad PSK — back off, app can re-provision
      return;
    }
  }

  if (!Cloud::ensureRegistered(g_creds)) {
    g_retryAt = millis() + 8000;  // bad/revoked API key or API down
    return;
  }

  // Open + service the realtime frame socket (idempotent). This is the high-fps transport: one held
  // wss connection the camera streams binary JPEG over, instead of one HTTPS POST per frame.
  if (!CloudWs::connected() && g_wsStartedAt == 0) g_wsStartedAt = now;
  CloudWs::begin(g_creds);
  CloudWs::loop();
  if (CloudWs::connected()) g_wsStartedAt = 0;  // healthy — reset the connect watchdog
  // The socket has been started but won't connect (expired token / deleted session / bad route): drop
  // the cached registration so the next pass re-registers and mints a fresh token + socket.
  else if (g_wsStartedAt && (now - g_wsStartedAt) > 25000) {
    Serial.println("[ws] never connected — clearing registration to re-register");
    CloudWs::stop();
    Store::clearRegistration();
    g_creds.sessionId = "";
    g_creds.deviceId = "";
    g_creds.token = "";
    g_wsStartedAt = 0;
    return;
  }

  // Control (sensor on/off + cadence) rides a low-rate HTTP poll, decoupled from the frame stream —
  // the WS frame path carries no per-frame response, so this is how a streaming camera learns about a
  // mic toggle or an fps change from the dashboard.
  if (now >= g_nextControl) {
    Cloud::Control ctl;
    if (Cloud::pollControl(g_creds, &ctl)) applyControl(ctl);
    g_nextControl = millis() + CAGI_CONTROL_POLL_MS;
  }

  // What can actually run = wired AND desired-on. Either sensor missing/off just drops out.
  const bool camActive = g_cameraOk && g_camDesired;
  const bool micActive = g_audioOk && g_micDesired;

  // Nothing to stream (no sensors, or the operator turned them all off): stay online; control poll above
  // already runs so a stopped camera can be remotely turned back on.
  if (!camActive && !micActive) {
    if (g_streaming) { Status::set("paused"); g_streaming = false; }
    delay(50);
    return;
  }

  bool didWork = false;

  // Camera frame → stream the JPEG bytes over the held WebSocket (no per-frame HTTP).
  if (camActive && now >= g_nextFrame && CloudWs::connected()) {
    uint8_t* buf = nullptr;
    size_t len = 0;
    if (Camera::capture(&buf, &len)) {
      g_captureFails = 0;
      bool sent = CloudWs::sendFrame(buf, len);
      Camera::release();
      if (sent) {
        noteStreamOk();
        didWork = true;
      }
    } else {
      // The sensor returned no frame. Surface it (this used to be silent — a black "starting camera"
      // screen with no clue why) and periodically re-init the OV2640 to recover from a wedged sensor
      // or a brownout (a common ESP32-CAM symptom when the board is under-powered).
      g_captureFails++;
      if (g_captureFails == 1 || g_captureFails % 10 == 0)
        Serial.printf("[cam] capture failed (x%u) — sensor returned no frame\n", g_captureFails);
      if (g_captureFails == 3) Status::set("camera_error", "no frames from sensor (power/wiring?)");
      if (g_captureFails % 15 == 0) {
        Serial.println("[cam] reinitializing sensor");
        g_cameraOk = Camera::reinit();
        if (!g_cameraOk) { Status::set("camera_error", "sensor re-init failed"); return; }
      }
    }
    g_nextFrame = millis() + g_frameIntervalMs;
  }

  // Mic clip (capture() blocks ~CAGI_AUDIO_CLIP_MS, which naturally paces audio back-to-back).
  if (micActive && now >= g_nextAudio) {
    uint8_t* abuf = nullptr;
    size_t alen = 0;
    if (Audio::capture(&abuf, &alen)) {
      Cloud::PostResult res;
      Cloud::Control ctl;
      Cloud::postAudio(g_creds, abuf, alen, &res, &ctl);
      Audio::release();
      if (handlePostFailure(res)) return;
      applyControl(ctl);
      noteStreamOk();
      didWork = true;
    }
    g_nextAudio = millis();  // next clip immediately (gapless) once the current one is sent
  }

#if CAGI_VERIFIED_SKU
  // Verified SKU: emit a signed capture manifest sidecar on its own cadence while the frame socket is
  // up. It describes the current window (fw, boot state, case-intact, live modalities) and is signed so
  // the claim can't be forged in transit; the platform re-verifies it and recomputes coherence from the
  // actual frames. Also poll the tamper switch here so a mid-stream opening is caught promptly.
  Tamper::poll();
  if (CloudWs::connected() && now >= g_nextManifest) {
    Manifest::emit(g_manifestSeq++);
    g_nextManifest = millis() + CAGI_MANIFEST_INTERVAL_MS;
  }
#endif

  if (didWork && !g_streaming) {
    Status::set("streaming");
    g_streaming = true;
  }
  delay(camActive ? 10 : 5);
}
