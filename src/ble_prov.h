#pragma once
#include <Arduino.h>

// BLE provisioning service — advertises the CommandAGI camera GATT service so the mobile/desktop app
// can recognize the device, read INFO, write Wi-Fi creds + account API key (PROVISION), and watch
// STATUS notifications. See packages/domain/core/src/esp32cam.ts for the wire contract.
namespace BleProv {
// Bring the BLE provisioning service up (idempotent — a no-op if already advertising).
void begin();
// Tear BLE all the way down (deinit the NimBLE controller) to FREE THE SHARED RADIO for Wi-Fi. The
// classic ESP32 forces Wi-Fi modem-sleep while BLE is active, so we stop BLE once streaming to let
// uploads run at full speed; begin() can bring it back for re-pairing if the stream drops.
void stop();
// Whether the BLE stack is currently up/advertising.
bool isUp();
// Report whether an INMP441 mic was detected, so INFO advertises `mic: true` and the app can show the
// camera as audio-capable. Call after Audio::begin(); refreshes the INFO characteristic if already up.
void setMicPresent(bool present);
// Returns true exactly once after the app writes a fresh, valid provisioning payload (creds are
// already persisted to NVS by then). The main loop polls this to (re)connect.
bool consumeNewProvisioning();
// Short 4-hex uppercase suffix of the device MAC — used in the advertised name + default device name.
String hwSuffix();
String hwid();  // full MAC string
}  // namespace BleProv
