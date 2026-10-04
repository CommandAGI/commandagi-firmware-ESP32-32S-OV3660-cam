#pragma once
#include <Arduino.h>
#include <functional>

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
};

// Register a callback invoked whenever the status changes (the BLE layer uses it to notify the app).
void onChange(std::function<void(const Snapshot&)> cb);

void set(const String& state, const String& error = "");
void setNetwork(const String& ip);
void setRegistration(const String& sessionId, const String& deviceId);

// Change the link / signal or the battery fields; notifies only when a value changed.
void setLink(const String& link, const String& op, int rssiDbm, int rsrpDbm10);
void setBattery(int mv, int pct, int charging);

Snapshot get();
String toJson();  // serialized Esp32CamStatus

}  // namespace Status
