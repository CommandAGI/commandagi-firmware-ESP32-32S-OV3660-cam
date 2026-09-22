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
};

// Register a callback invoked whenever the status changes (the BLE layer uses it to notify the app).
void onChange(std::function<void(const Snapshot&)> cb);

void set(const String& state, const String& error = "");
void setNetwork(const String& ip);
void setRegistration(const String& sessionId, const String& deviceId);

Snapshot get();
String toJson();  // serialized Esp32CamStatus

}  // namespace Status
