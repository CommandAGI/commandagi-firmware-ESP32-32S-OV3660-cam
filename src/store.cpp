#include "store.h"
#include <Preferences.h>
#include "config.h"

namespace {
Preferences prefs;
}

namespace Store {

void begin() {
  // Open once read/write; individual ops re-open as needed.
  prefs.begin(CAGI_NVS_NAMESPACE, /*readOnly=*/false);
  prefs.end();
}

bool hasCreds() {
  prefs.begin(CAGI_NVS_NAMESPACE, true);
  bool ok = prefs.isKey("ssid") && prefs.isKey("apiKey");
  prefs.end();
  return ok;
}

Creds load() {
  Creds c;
  prefs.begin(CAGI_NVS_NAMESPACE, true);
  c.ssid = prefs.getString("ssid", "");
  c.psk = prefs.getString("psk", "");
  c.apiBaseUrl = prefs.getString("base", "https://api.commandagi.com");
  c.apiKey = prefs.getString("apiKey", "");
  c.deviceName = prefs.getString("name", "");
  c.sessionId = prefs.getString("sid", "");
  c.deviceId = prefs.getString("did", "");
  c.token = prefs.getString("tok", "");
  prefs.end();
  return c;
}

void saveProvisioning(const Creds& c) {
  prefs.begin(CAGI_NVS_NAMESPACE, false);
  prefs.putString("ssid", c.ssid);
  prefs.putString("psk", c.psk);
  prefs.putString("base", c.apiBaseUrl);
  prefs.putString("apiKey", c.apiKey);
  prefs.putString("name", c.deviceName);
  // New creds → drop any cached session/device/token so we re-register cleanly.
  prefs.remove("sid");
  prefs.remove("did");
  prefs.remove("tok");
  prefs.end();
}

void saveRegistration(const String& sessionId, const String& deviceId, const String& token) {
  prefs.begin(CAGI_NVS_NAMESPACE, false);
  prefs.putString("sid", sessionId);
  prefs.putString("did", deviceId);
  prefs.putString("tok", token);
  prefs.end();
}

void clearRegistration() {
  prefs.begin(CAGI_NVS_NAMESPACE, false);
  prefs.remove("sid");
  prefs.remove("did");
  prefs.remove("tok");
  prefs.end();
}

void factoryReset() {
  prefs.begin(CAGI_NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
}

}  // namespace Store
