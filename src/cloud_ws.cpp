#include "cloud_ws.h"
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "status.h"
#include "speaker.h"
#include "clip.h"
#if CAGI_DEVICE_SEALS
#include <esp_heap_caps.h>
#include "mbedtls/base64.h"
#endif

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

// A message larger than WEBSOCKETS_MAX_DATA_SIZE (15 kB in links2004/WebSockets 2.x, not overridable)
// makes the library close the socket with 1009. So a base64 clip must keep the whole control message
// under 15 kB; a larger clip must come as a `url`.

// Tell the DO our display channel so a realtime camera view appears (and the recorder knows the kind).
// A sealing build declares each stream as a channel of the device that seals it itself: the recorder
// then keeps the device's frames and lines byte for byte (docs/integrity.md in CommandAGI).
void announceChannel() {
  JsonDocument d;
  d["type"] = "channels";
  JsonArray arr = d["channels"].to<JsonArray>();
  JsonObject c0 = arr.add<JsonObject>();
  c0["channelId"] = CAGI_STREAM_CHANNEL;  // "cam"
  c0["kind"] = "camera";
  c0["label"] = g_channelName;
#if CAGI_DEVICE_SEALS
  JsonObject ch = c0["channel"].to<JsonObject>();
  ch["id"] = CAGI_STREAM_CHANNEL;
  ch["dir"] = "out";
  ch["medium"] = "video";
  ch["format"] = "video";
  ch["seals"] = "device";
  JsonObject c1 = arr.add<JsonObject>();
  c1["channelId"] = CAGI_SEAL_INDEX_CHANNEL;  // "cam-at": one line per frame, and the seals
  c1["kind"] = "events";
  c1["label"] = "Frame index";
  JsonObject ix = c1["channel"].to<JsonObject>();
  ix["id"] = CAGI_SEAL_INDEX_CHANNEL;
  ix["dir"] = "out";
  ix["medium"] = "records";
  ix["format"] = "byte-ranges";
  ix["seals"] = "device";
#endif
  String out;
  serializeJson(d, out);
  ws.sendTXT(out);
}

#if CAGI_DEVICE_SEALS
// One growing PSRAM buffer for a message: prefix, base64 of the bytes, suffix. Only the loop's task
// sends, so one buffer is enough.
uint8_t* g_msg = nullptr;
size_t g_msgCap = 0;

bool sendBase64(const char* prefix, const uint8_t* bytes, size_t len, const char* suffix) {
  const size_t pre = strlen(prefix), suf = strlen(suffix), b64 = (len + 2) / 3 * 4;
  const size_t need = pre + b64 + suf + 1;
  if (need > g_msgCap) {
    free(g_msg);
    g_msgCap = need + 16 * 1024;
    g_msg = (uint8_t*)heap_caps_malloc(g_msgCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_msg) g_msg = (uint8_t*)malloc(g_msgCap);
    if (!g_msg) {
      g_msgCap = 0;
      return false;
    }
  }
  memcpy(g_msg, prefix, pre);
  size_t n = 0;
  if (mbedtls_base64_encode(g_msg + pre, g_msgCap - pre, &n, bytes, len) != 0) return false;
  memcpy(g_msg + pre + n, suffix, suf);
  return ws.sendTXT(g_msg, pre + n + suf);
}
#endif

#if CAGI_SPEAKER_ENABLED
// The speaker as an output plus the presence controls that drive it. The schemas are the ones
// packages/runtime/host-core/src/client.ts declares (PRESENCE_PAYLOAD_SCHEMAS). No `say`: this device
// has no text-to-speech; the platform turns `say` into a clip and sends `play_audio`.
const char kOutputs[] =
    R"({"type":"outputs","outputs":[{"outputId":"speaker","kind":"speaker","label":"Speaker","primary":true}]})";
const char kControls[] =
    R"({"type":"controls","controls":[{"channelId":"ctrl","kind":"ctrl","label":"Presence",)"
    R"("actions":["play_audio","present_stop"],"payloadSchema":{)"
    R"("play_audio":{"type":"object","properties":{"url":{"type":"string","maxLength":2097152},)"
    R"("base64":{"type":"string","maxLength":16777216},"mime":{"type":"string","maxLength":256},)"
    R"("format":{"type":"string","maxLength":256},"outputId":{"type":"string","maxLength":256},)"
    R"("loop":{"type":"boolean"},"interrupt":{"type":"boolean"}},"additionalProperties":false},)"
    R"("present_stop":{"type":"object","properties":{"outputId":{"type":"string","maxLength":256}},)"
    R"("additionalProperties":false}}}]})";

void sendResult(const String& requestId, const char* action, bool ok, uint32_t durationMs, const String& error) {
  if (!requestId.length()) return;  // a command without a requestId wants no reply
  JsonDocument d;
  d["type"] = "action_result";
  d["requestId"] = requestId;
  d["action"] = action;
  JsonObject r = d["result"].to<JsonObject>();
  r["ok"] = ok;
  if (ok && durationMs) r["durationMs"] = durationMs;
  if (!ok) r["error"] = error;
  String out;
  serializeJson(d, out);
  ws.sendTXT(out);
}

// One control message for the speaker. Every play_audio / present_stop with a requestId gets exactly
// one action_result: now (refused, or present_stop) or later from Speaker::pollResult().
void onControl(JsonDocument& msg) {
  const String action = msg["action"] | "";
  const String requestId = msg["requestId"] | "";
  JsonVariant p = msg["payload"];
  const String channelId = msg["channelId"] | "";
  const String payloadChannel = p["channelId"] | "";
  if ((channelId.length() && channelId != "ctrl") || (payloadChannel.length() && payloadChannel != "ctrl")) {
    sendResult(requestId, action.c_str(), false, 0, "control '" + action + "' is not declared at '" + channelId + "'");
    return;
  }
  const String outputId = p["outputId"] | "speaker";
  if (outputId != "speaker") {
    sendResult(requestId, action.c_str(), false, 0, "unknown output '" + outputId + "'");
    return;
  }
  if (action == "present_stop") {
    Speaker::stop();
    Serial.println("[spk] present_stop");
    sendResult(requestId, "present_stop", true, 0, "");
    return;
  }
  if (action != "play_audio") {
    sendResult(requestId, action.c_str(), false, 0, "control '" + action + "' is not declared at 'ctrl'");
    return;
  }
  if (p["loop"] | false) {
    sendResult(requestId, "play_audio", false, 0, "loop is not supported");
    return;
  }
  Speaker::Request r;
  r.requestId = requestId;
  r.url = p["url"] | "";
  r.hint = p["format"] | "";
  if (!r.hint.length()) r.hint = p["mime"] | "";
  r.interrupt = p["interrupt"] | true;
  const char* b64 = p["base64"] | (const char*)nullptr;
  if (!r.url.length() && b64) {
    const size_t inLen = strlen(b64);
    const size_t cap = inLen / 4 * 3 + 3;
    r.bytes = (uint8_t*)malloc(cap);
    if (!r.bytes) {
      sendResult(requestId, "play_audio", false, 0, "out of memory");
      return;
    }
    if (!Clip::base64Decode(b64, inLen, r.bytes, cap, &r.len)) {
      free(r.bytes);
      sendResult(requestId, "play_audio", false, 0, "bad base64");
      return;
    }
  }
  if (const char* why = Speaker::play(r)) sendResult(requestId, "play_audio", false, 0, why);
}

void onText(const uint8_t* payload, size_t len) {
  JsonDocument msg;
  if (deserializeJson(msg, payload, len)) return;
  const String type = msg["type"] | "";
  if (type == "control") {
    onControl(msg);
  } else if (type == "config") {
    // A config envelope replaces the output switches as a whole: a missing `speaker` means enabled.
    JsonVariant outs = msg["outputs"];
    if (outs.is<JsonObject>()) Speaker::setEnabled(outs["speaker"] | true);
  }
}
#endif

void onEvent(WStype_t type, uint8_t* payload, size_t len) {
  (void)payload;
  (void)len;
  switch (type) {
    case WStype_CONNECTED:
      g_connected = true;
      Serial.println("[ws] connected — announcing camera channel");
      announceChannel();
#if CAGI_SPEAKER_ENABLED
      if (Speaker::available()) {
        ws.sendTXT(kOutputs);
        ws.sendTXT(kControls);
      }
#endif
      ::Status::set("streaming");
      break;
#if CAGI_SPEAKER_ENABLED
    case WStype_TEXT:
      onText(payload, len);
      break;
#endif
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
  Speaker::setAuth(c.apiBaseUrl, c.apiKey);

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
#if CAGI_SPEAKER_ENABLED
  // Results come from the speaker task; only this (the loop's) task writes to the socket. A result
  // made while the socket is down is lost: the platform sees no action_result for that requestId.
  Speaker::Result r;
  while (Speaker::pollResult(&r)) {
    if (g_connected) sendResult(r.requestId, "play_audio", r.ok, r.durationMs, r.error);
  }
#endif
}

bool connected() { return g_started && g_connected; }

bool sendFrame(const uint8_t* buf, size_t len) {
  if (!g_started || !g_connected) return false;
  return ws.sendBIN(buf, len);
}

#if CAGI_DEVICE_SEALS
bool sendFrameJson(const char* channelId, const uint8_t* jpeg, size_t len) {
  if (!g_started || !g_connected) return false;
  char prefix[128];
  snprintf(prefix, sizeof prefix, "{\"type\":\"frame\",\"channelId\":\"%s\",\"kind\":\"camera\",\"url\":\"data:image/jpeg;base64,", channelId);
  return sendBase64(prefix, jpeg, len, "\"}");
}

bool sendLines(const char* channelId, const char* kind, const char* lines, size_t len) {
  if (!g_started || !g_connected) return false;
  char prefix[160];
  snprintf(prefix, sizeof prefix, "{\"type\":\"data\",\"channelId\":\"%s\",\"kind\":\"%s\",\"format\":\"jsonl\",\"url\":\"data:text/plain;base64,", channelId, kind);
  return sendBase64(prefix, (const uint8_t*)lines, len, "\"}");
}
#endif

bool sendText(const String& json) {
  if (!g_started || !g_connected) return false;
  return ws.sendTXT(json.c_str(), json.length());
}

void stop() {
  if (!g_started) return;
  ws.disconnect();
  g_started = false;
  g_connected = false;
}

}  // namespace CloudWs
