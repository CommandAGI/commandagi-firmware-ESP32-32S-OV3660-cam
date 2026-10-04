#pragma once
#include <Arduino.h>
#include <functional>
#include <ArduinoJson.h>
#include "config.h"

// Lifecycle state — string values MUST match ESP32_CAM_STATES in packages/domain/core/src/esp32cam.ts.
namespace Status {

struct Snapshot {
  String state = "idle";
  String ip;
  String sessionId;
  String deviceId;
  String error;
  // Link and battery: set only by cellular / battery builds, so other builds keep their exact STATUS.
  String link;        // wifi | cellular | "" (none yet)
  String op;          // cellular operator
  int rssiDbm = 0;    // 0 = unknown
  int rsrpDbm10 = 0;  // RSRP x10; 0 = unknown
  int batMv = -1;     // -1 = no battery reading
  int batPct = -1;
  int charging = -1;  // 1 / 0; -1 = unknown
#if CAGI_IR_ENABLED
  // IR night mode. irMode "" = not set yet.
  String irMode;       // the operator's desired mode: auto | on | off
  bool irOn = false;   // the level the firmware drives on the IR enable pin
  float irLux = -1;    // < 0 = no light reading
  bool irNight = false;
#endif
};

// Register a callback invoked whenever the status changes (the BLE layer uses it to notify the app).
void onChange(std::function<void(const Snapshot&)> cb);

void set(const String& state, const String& error = "");
void setNetwork(const String& ip);
void setRegistration(const String& sessionId, const String& deviceId);

// Change the link / signal or the battery fields; notifies only when a value changed.
void setLink(const String& link, const String& op, int rssiDbm, int rsrpDbm10);
void setBattery(int mv, int pct, int charging);
#if CAGI_IR_ENABLED
// Change the IR fields; notifies when mode, on or night changed, or the light moved by more than 25 %.
void setIr(const char* mode, bool on, float lux, bool night);
#endif

Snapshot get();
String toJson();  // serialized Esp32CamStatus
#if CAGI_IR_ENABLED
// `{ mode, on, lux?, night }` into out (BLE STATUS and the runtime `status` message share it).
void irJson(const Snapshot& s, JsonObject out);
#endif

}  // namespace Status
