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

void setLink(const String& link, const String& op, int rssiDbm, int rsrpDbm10) {
  if (g_snap.link == link && g_snap.op == op && g_snap.rssiDbm == rssiDbm && g_snap.rsrpDbm10 == rsrpDbm10) return;
  if (g_snap.link != link) Serial.printf("[status] link %s\n", link.length() ? link.c_str() : "none");
  g_snap.link = link;
  g_snap.op = op;
  g_snap.rssiDbm = rssiDbm;
  g_snap.rsrpDbm10 = rsrpDbm10;
  emit();
}

void setBattery(int mv, int pct, int charging) {
  // 50 mV hysteresis: the ADC jitters, and each change is a BLE notification.
  if (abs(g_snap.batMv - mv) < 50 && g_snap.batPct == pct && g_snap.charging == charging) return;
  g_snap.batMv = mv;
  g_snap.batPct = pct;
  g_snap.charging = charging;
  emit();
}

#if CAGI_IR_ENABLED
void setIr(const char* mode, bool on, float lux, bool night) {
  const bool same = g_snap.irMode == mode && g_snap.irOn == on && g_snap.irNight == night;
  // Each STATUS change is a BLE notification; the light is read every 2 s and moves a little each time.
  const float was = g_snap.irLux;
  const bool luxMoved = (was < 0) != (lux < 0) || fabsf(lux - was) > 0.25f * fmaxf(was, 4.0f);
  if (same && !luxMoved) return;
  g_snap.irMode = mode;
  g_snap.irOn = on;
  g_snap.irLux = lux;
  g_snap.irNight = night;
  emit();
}

void irJson(const Snapshot& s, JsonObject out) {
  out["mode"] = s.irMode;
  out["on"] = s.irOn;
  if (s.irLux >= 0) out["lux"] = roundf(s.irLux * 10) / 10;
  out["night"] = s.irNight;
}
#endif

Snapshot get() { return g_snap; }

String toJson() {
  JsonDocument doc;
  doc["state"] = g_snap.state;
  if (g_snap.ip.length()) doc["ip"] = g_snap.ip;
  if (g_snap.sessionId.length()) doc["sessionId"] = g_snap.sessionId;
  if (g_snap.deviceId.length()) doc["deviceId"] = g_snap.deviceId;
  if (g_snap.error.length()) doc["error"] = g_snap.error;
  if (g_snap.link.length()) doc["link"] = g_snap.link;
  if (g_snap.op.length()) doc["operator"] = g_snap.op;
  if (g_snap.rssiDbm) doc["rssi"] = g_snap.rssiDbm;
  if (g_snap.rsrpDbm10) doc["rsrp"] = g_snap.rsrpDbm10 / 10.0;
  if (g_snap.batMv >= 0) {
    JsonObject b = doc["battery"].to<JsonObject>();
    b["mv"] = g_snap.batMv;
    if (g_snap.batPct >= 0) b["pct"] = g_snap.batPct;
    if (g_snap.charging >= 0) b["charging"] = g_snap.charging == 1;
  }
#if CAGI_IR_ENABLED
  if (g_snap.irMode.length()) irJson(g_snap, doc["ir"].to<JsonObject>());
#endif
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace Status
