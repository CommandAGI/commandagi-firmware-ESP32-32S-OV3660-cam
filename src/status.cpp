#include "status.h"
#include <ArduinoJson.h>

namespace {
Status::Snapshot g_snap;
std::function<void(const Status::Snapshot&)> g_cb;

void emit() {
  if (g_cb) g_cb(g_snap);
}
}  // namespace

namespace Status {

void onChange(std::function<void(const Snapshot&)> cb) { g_cb = std::move(cb); }

void set(const String& state, const String& error) {
  g_snap.state = state;
  g_snap.error = error;
  Serial.printf("[status] %s%s%s\n", state.c_str(), error.length() ? " — " : "", error.c_str());
  emit();
}

void setNetwork(const String& ip) {
  g_snap.ip = ip;
  emit();
}

void setRegistration(const String& sessionId, const String& deviceId) {
  g_snap.sessionId = sessionId;
  g_snap.deviceId = deviceId;
  emit();
}

Snapshot get() { return g_snap; }

String toJson() {
  JsonDocument doc;
  doc["state"] = g_snap.state;
  if (g_snap.ip.length()) doc["ip"] = g_snap.ip;
  if (g_snap.sessionId.length()) doc["sessionId"] = g_snap.sessionId;
  if (g_snap.deviceId.length()) doc["deviceId"] = g_snap.deviceId;
  if (g_snap.error.length()) doc["error"] = g_snap.error;
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace Status
