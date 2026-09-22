#pragma once
#include <Arduino.h>

// Persistent provisioning state, stored in the `cagi` NVS namespace. A factory-reset clears it all
// (but never touches the firmware itself — see partitions.csv).
struct Creds {
  String ssid;
  String psk;
  String apiBaseUrl;  // e.g. https://api.commandagi.com
  String apiKey;      // per-device cagi_ user key — "logged in under your account"
  String deviceName;
  // Cached session/device the camera registered, so a reboot reuses them instead of creating a new
  // machine every time. Cleared (and re-registered) if the API later 404s them.
  String sessionId;
  String deviceId;
  // World-scoped runtime token from connect-device — authenticates the realtime WebSocket (the MJPEG
  // frame stream). Long-lived (~30d); re-minted on re-registration.
  String token;
};

namespace Store {
void begin();
bool hasCreds();                 // true once an SSID + apiKey are stored
Creds load();
void saveProvisioning(const Creds& c);   // ssid/psk/base/key/name (clears cached session/device)
void saveRegistration(const String& sessionId, const String& deviceId, const String& token);
void clearRegistration();        // forget the cached session/device only
void factoryReset();             // erase the whole namespace (creds + registration)
}  // namespace Store
