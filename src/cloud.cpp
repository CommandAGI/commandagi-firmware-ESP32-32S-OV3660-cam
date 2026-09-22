#include "cloud.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "status.h"
#include "ble_prov.h"

namespace {
// One reusable TLS client for all cloud calls (keeps the handshake warm between frames).
WiFiClientSecure g_tls;
bool g_tlsReady = false;

void ensureTls() {
  if (g_tlsReady) return;
  // NOTE: setInsecure() skips server-cert validation. It's the pragmatic default for a hobby camera
  // on your own LAN and survives Cloudflare cert rotation. To harden, pin the ISRG Root X1 CA here
  // with g_tls.setCACert(ISRG_ROOT_X1) instead. The API key still authenticates the *device*.
  g_tls.setInsecure();
  g_tlsReady = true;
}
}  // namespace

namespace Cloud {

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

bool connectWifi(const Creds& c) {
  Status::set("wifi_connecting");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);  // ride out brief AP/RSSI dropouts without a full reconnect cycle
  // The classic ESP32 shares ONE radio between Wi-Fi and BLE. While BLE is active the coexistence layer
  // REQUIRES modem-sleep (disabling it aborts: "Should enable WiFi modem sleep when both WiFi and
  // Bluetooth are enabled"). So gate it on BLE: sleep while BLE is up (provisioning), full-speed once
  // BLE has been stopped for streaming (see noteStreamOk()).
  WiFi.setSleep(BleProv::isUp());
  WiFi.begin(c.ssid.c_str(), c.psk.length() ? c.psk.c_str() : nullptr);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Status::set("wifi_failed", "could not join " + c.ssid);
    return false;
  }
  Status::setNetwork(WiFi.localIP().toString());
  Serial.printf("[wifi] connected: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// Small helper: authenticated JSON POST, returns HTTP status (or <0 on transport error) and fills body.
static int postJson(const String& url, const String& apiKey, const String& bodyIn, String& bodyOut) {
  ensureTls();
  HTTPClient http;
  http.setReuse(true);
  if (!http.begin(g_tls, url)) return -1;
  http.addHeader("Authorization", "Bearer " + apiKey);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)bodyIn.c_str(), bodyIn.length());
  bodyOut = (code > 0) ? http.getString() : "";
  http.end();
  return code;
}

// Parse a `{ ..., "sensors": { "cam": bool, "mic": bool } }` body into a Control. A missing key keeps
// that sensor's default (on); a missing `sensors` object leaves ctl->valid false (no opinion).
static void parseControl(const String& body, Cloud::Control* ctl) {
  if (!ctl || body.length() == 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  // Server-set frame cadence (operator/app-configurable) — adopt it so the camera's frame rate is
  // controllable from the dashboard instead of being pinned to the compile-time default.
  if (doc["intervalMs"].is<unsigned long>() || doc["intervalMs"].is<int>()) {
    ctl->intervalMs = doc["intervalMs"].as<unsigned long>();
    ctl->valid = true;
  }
  JsonVariant s = doc["sensors"];
  if (!s.is<JsonObject>()) return;
  ctl->valid = true;
  if (s["cam"].is<bool>()) ctl->cam = s["cam"].as<bool>();
  if (s["mic"].is<bool>()) ctl->mic = s["mic"].as<bool>();
}

// One media POST (image/audio). Fills *res (ok/gone/authFailed/code) and parses the response into ctl
// on success. Logs every non-2xx with its HTTP code so a stuck camera is always diagnosable over
// serial (the old version swallowed failures, which is how a bad key looked identical to a hiccup).
static bool postMedia(const Creds& c, const String& url, const char* contentType, const uint8_t* buf,
                      size_t len, Cloud::PostResult* res, Cloud::Control* ctl) {
  Cloud::PostResult local;
  Cloud::PostResult* r = res ? res : &local;
  *r = Cloud::PostResult{};
  ensureTls();
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(8000);  // bound the call so a black-hole network can't wedge the loop indefinitely
  if (!http.begin(g_tls, url)) {
    r->code = -1;
    Serial.println("[cloud] frame POST: begin() failed (TLS/URL)");
    return false;
  }
  http.addHeader("Authorization", "Bearer " + c.apiKey);
  http.addHeader("Content-Type", contentType);
  int code = http.POST((uint8_t*)buf, len);
  String body = (code > 0) ? http.getString() : "";
  http.end();
  r->code = code;
  r->gone = (code == 404);                       // session/device deleted server-side
  r->authFailed = (code == 401 || code == 403);  // bad/revoked/expired API key
  r->ok = code >= 200 && code < 300;
  if (r->ok) parseControl(body, ctl);
  else Serial.printf("[cloud] frame POST → %d\n", code);
  return r->ok;
}

bool ensureRegistered(Creds& c) {
  if (c.sessionId.length() && c.deviceId.length()) return true;  // reuse cached registration

  Status::set("registering");
  const String name = c.deviceName.length() ? c.deviceName : String("My camera");

  // 1) Create the agentless world session.
  String body;
  {
    JsonDocument d;
    d["title"] = name;
    String req;
    serializeJson(d, req);
    int code = postJson(c.apiBaseUrl + "/worlds", c.apiKey, req, body);
    if (code != 200) {
      Status::set("register_failed", "POST /worlds → " + String(code));
      return false;
    }
    JsonDocument resp;
    if (deserializeJson(resp, body) || !resp["sessionId"].is<const char*>()) {
      Status::set("register_failed", "bad /worlds response");
      return false;
    }
    c.sessionId = String((const char*)resp["sessionId"]);
  }

  // 2) Connect this device as a camera (kind="camera" → byo.camera world).
  {
    JsonDocument d;
    d["kind"] = "camera";
    d["name"] = name;
    String req;
    serializeJson(d, req);
    int code = postJson(c.apiBaseUrl + "/sessions/" + c.sessionId + "/connect-device", c.apiKey, req, body);
    if (code != 200) {
      Status::set("register_failed", "connect-device → " + String(code));
      c.sessionId = "";
      return false;
    }
    JsonDocument resp;
    if (deserializeJson(resp, body) || !resp["deviceId"].is<const char*>()) {
      Status::set("register_failed", "bad connect-device response");
      c.sessionId = "";
      return false;
    }
    c.deviceId = String((const char*)resp["deviceId"]);
    // The world-scoped runtime token authenticates the realtime WebSocket (the MJPEG frame stream).
    c.token = resp["token"].is<const char*>() ? (const char*)resp["token"] : "";
  }

  Store::saveRegistration(c.sessionId, c.deviceId, c.token);
  Status::setRegistration(c.sessionId, c.deviceId);
  Serial.printf("[cloud] registered session=%s device=%s\n", c.sessionId.c_str(), c.deviceId.c_str());
  return true;
}

bool postFrame(const Creds& c, const uint8_t* buf, size_t len, unsigned long intervalMs, PostResult* res, Control* ctl) {
  String url = c.apiBaseUrl + "/v1/streams/" + c.sessionId + "/" + c.deviceId +
               "/frame?channel=" + CAGI_STREAM_CHANNEL + "&kind=camera&intervalMs=" + String(intervalMs);
  return postMedia(c, url, "image/jpeg", buf, len, res, ctl);
}

bool postAudio(const Creds& c, const uint8_t* buf, size_t len, PostResult* res, Control* ctl) {
  String url = c.apiBaseUrl + "/v1/streams/" + c.sessionId + "/" + c.deviceId +
               "/frame?channel=" + CAGI_AUDIO_CHANNEL + "&kind=audio&intervalMs=" + String(CAGI_AUDIO_CLIP_MS);
  return postMedia(c, url, "audio/wav", buf, len, res, ctl);
}

bool pollControl(const Creds& c, Control* ctl) {
  if (!ctl) return false;
  ensureTls();
  String url = c.apiBaseUrl + "/v1/streams/" + c.sessionId + "/" + c.deviceId + "/control";
  HTTPClient http;
  http.setReuse(true);
  if (!http.begin(g_tls, url)) return false;
  http.addHeader("Authorization", "Bearer " + c.apiKey);
  int code = http.GET();
  String body = (code > 0) ? http.getString() : "";
  http.end();
  if (code < 200 || code >= 300) return false;
  parseControl(body, ctl);
  return true;
}

}  // namespace Cloud
