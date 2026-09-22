#include "cloud_ws.h"
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "status.h"

namespace {
WebSocketsClient ws;
bool g_started = false;
bool g_connected = false;
String g_channelName;

// "https://api.commandagi.com" → "api.commandagi.com" (drop scheme, any path, any :port).
String hostFromBase(const String& base) {
  String h = base;
  h.replace("https://", "");
  h.replace("http://", "");
  int slash = h.indexOf('/');
  if (slash >= 0) h = h.substring(0, slash);
  int colon = h.indexOf(':');
  if (colon >= 0) h = h.substring(0, colon);
  return h;
}

// Tell the DO our display channel so a realtime camera view appears (and the recorder knows the kind).
void announceChannel() {
  JsonDocument d;
  d["type"] = "channels";
  JsonArray arr = d["channels"].to<JsonArray>();
  JsonObject c0 = arr.add<JsonObject>();
  c0["channelId"] = CAGI_STREAM_CHANNEL;  // "cam"
  c0["kind"] = "camera";
  c0["label"] = g_channelName;
  String out;
  serializeJson(d, out);
  ws.sendTXT(out);
}

void onEvent(WStype_t type, uint8_t* payload, size_t len) {
  (void)payload;
  (void)len;
  switch (type) {
    case WStype_CONNECTED:
      g_connected = true;
      Serial.println("[ws] connected — announcing camera channel");
      announceChannel();
      ::Status::set("streaming");
      break;
    case WStype_DISCONNECTED:
      if (g_connected) Serial.println("[ws] disconnected");
      g_connected = false;
      break;
    case WStype_ERROR:
      g_connected = false;
      break;
    default:
      break;  // inbound TEXT/BIN/PONG — control still arrives via the HTTP poll in cloud.cpp
  }
}
}  // namespace

namespace CloudWs {

void begin(const Creds& c) {
  if (g_started) return;
  if (c.sessionId.isEmpty() || c.deviceId.isEmpty() || c.token.isEmpty()) return;  // not registered yet
  // The channel label describes the FEED, not the unit — the device name already identifies the unit,
  // so echoing it here just renders as "<name> · <name>". A fixed "Camera" reads correctly if a unit
  // ever exposes more than one channel ("<name> · Camera").
  g_channelName = "Camera";

  const String host = hostFromBase(c.apiBaseUrl);
  // Same realtime route the host-core runtime uses; token chars are URL-safe (base64url + '.').
  const String path = "/rt/run/" + c.sessionId + "?device=" + c.deviceId + "&runtime=1&role=agent&token=" + c.token;

  // wss on 443. NOTE: like the HTTP path (g_tls.setInsecure), this rides Cloudflare's cert without
  // pinning — arduinoWebSockets' ESP32 SSL client connects without a CA here; if a build enforces
  // verification, switch to ws.beginSslWithCA(host, 443, path, ISRG_ROOT_X1). The token authenticates us.
  ws.beginSSL(host.c_str(), 443, path.c_str());
  ws.onEvent(onEvent);
  ws.setReconnectInterval(3000);          // auto-redial a dropped socket
  ws.enableHeartbeat(15000, 3000, 2);     // ping every 15s; drop after 2 missed pongs
  g_started = true;
  Serial.printf("[ws] connecting wss://%s%s\n", host.c_str(), ("/rt/run/" + c.sessionId).c_str());
}

void loop() {
  if (g_started) ws.loop();
}

bool connected() { return g_started && g_connected; }

bool sendFrame(const uint8_t* buf, size_t len) {
  if (!g_started || !g_connected) return false;
  return ws.sendBIN(buf, len);
}

#if CAGI_VERIFIED_SKU
bool sendManifest(const String& json) {
  if (!g_started || !g_connected) return false;
  return ws.sendTXT(json);
}
#endif

void stop() {
  if (!g_started) return;
  ws.disconnect();
  g_started = false;
  g_connected = false;
}

}  // namespace CloudWs
