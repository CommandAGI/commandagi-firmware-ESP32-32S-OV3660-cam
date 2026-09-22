#include "manifest.h"

#if CAGI_VERIFIED_SKU
#include <ArduinoJson.h>
#include <Preferences.h>
#include "crypto.h"
#include "tamper.h"
#include "cloud_ws.h"
#if CAGI_SENSOR_LIDAR
#include "sensors/lidar.h"
#endif
#if CAGI_SENSOR_THERMAL
#include "sensors/thermal.h"
#endif
#if CAGI_SENSOR_EMI
#include "sensors/emi.h"
#endif

namespace {
bool g_begun = false;
bool g_haveSeed = false;
uint8_t g_seed[32] = {0};  // device Ed25519 seed (non-SE build); zeroed + unused on the SE build
String g_signerHex;        // cached public key (hex) — the manifest `signer`

// NVS key the provisioning path persists DeviceProvisioning.devicePrivKeyHex under (64 hex chars → 32
// bytes). TODO(provisioning): ble_prov.cpp must write this on a verified-SKU provision so the manifest
// can be signed; until then a non-SE verified build has no seed and emit() no-ops (fails closed).
constexpr const char* kSeedKey = "devseed";

bool hexToBytes32(const String& hex, uint8_t out[32]) {
  if (hex.length() != 64) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (int i = 0; i < 32; i++) {
    int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// The active modalities. Always "rgb" (the camera); each present corroborating sensor adds its tag.
// Kept in a fixed declared order so the signed message pre-image is deterministic (matches the JSON
// array order the platform re-derives the signing message from).
void activeModalities(String out[4], uint8_t& n) {
  n = 0;
  out[n++] = "rgb";
#if CAGI_SENSOR_LIDAR
  if (Sensors::Lidar::present()) out[n++] = "lidar";
#endif
#if CAGI_SENSOR_THERMAL
  if (Sensors::Thermal::present()) out[n++] = "ir";
#endif
#if CAGI_SENSOR_EMI
  if (Sensors::Emi::present()) out[n++] = "emi";
#endif
}
}  // namespace

namespace Manifest {

void begin() {
  if (g_begun) return;
#if CAGI_SECURE_ELEMENT
  // The key lives in the SE — no seed to load; the public key is read from the SE.
  g_signerHex = Crypto::ed25519PubKeyHex(nullptr);
  g_haveSeed = g_signerHex.length() == 64;  // "have a usable signer" — SE ready
#else
  Preferences p;
  if (p.begin(CAGI_NVS_NAMESPACE, /*readOnly=*/true)) {
    String hex = p.getString(kSeedKey, "");
    p.end();
    if (hexToBytes32(hex, g_seed)) {
      g_haveSeed = true;
      g_signerHex = Crypto::ed25519PubKeyHex(g_seed);
    }
  }
#endif
  g_begun = true;
  Serial.printf("[manifest] begin — signer %s\n", g_haveSeed ? "ready" : "UNAVAILABLE (emit no-ops)");
}

void emit(uint32_t seq) {
  if (!g_begun) begin();
  if (!g_haveSeed) return;  // fail closed: no manifest rather than an unsigned/forgeable one

  Tamper::poll();  // fold in any breach that happened since the last emit
  const bool caseIntact = Tamper::tamper_case_intact();

  // We never burn Secure Boot eFuses (the board must stay re-flashable — docs/platform/CAMERAS.md), so
  // the standard verified build cannot claim measured boot: report "unverified" honestly. Only a build
  // that has a genuine measured-boot attestation (e.g. via the SE) should report "secure".
  const char* bootState = "unverified";

  String mods[4];
  uint8_t nMods = 0;
  activeModalities(mods, nMods);
  String modsJoined;
  for (uint8_t i = 0; i < nMods; i++) {
    if (i) modsJoined += ",";
    modsJoined += mods[i];
  }

  // Device self-coherence is only a HINT (the platform recomputes it from the frames). The device
  // cannot honestly grade its own capture, so we report a neutral 1 ("no self-detected inconsistency").
  // Format as "1" to match JS Number→string of 1 (the platform re-derives the signing message from the
  // parsed JSON number, so the string forms must agree).
  const char* selfCoherenceStr = "1";

  // Domain-separated signing message — MUST match manifestSigningMessage() in integrity.ts exactly.
  String msg = String("cagi-manifest:v1|") + seq + "|" + CAGI_FW_VERSION + "|" + bootState + "|" +
               (caseIntact ? "1" : "0") + "|" + modsJoined + "|" + selfCoherenceStr;

  String sigHex = Crypto::ed25519SignHex((const uint8_t*)msg.c_str(), msg.length(), g_seed);
  if (sigHex.length() != 128) {
    Serial.println("[manifest] sign failed — skipping emit");
    return;
  }

  JsonDocument d;
  d["type"] = "manifest";
  d["channelId"] = CAGI_MANIFEST_CHANNEL;
  JsonObject m = d["manifest"].to<JsonObject>();
  m["seq"] = seq;
  m["fw"] = CAGI_FW_VERSION;
  m["bootState"] = bootState;
  m["caseIntact"] = caseIntact;
  JsonArray ma = m["modalities"].to<JsonArray>();
  for (uint8_t i = 0; i < nMods; i++) ma.add(mods[i]);
  m["selfCoherence"] = 1;  // matches selfCoherenceStr above
  m["signer"] = g_signerHex;
  m["signature"] = sigHex;

  String out;
  serializeJson(d, out);
  CloudWs::sendManifest(out);
}

}  // namespace Manifest

#else  // CAGI_VERIFIED_SKU == 0 — stock camera, no manifest

namespace Manifest {
void begin() {}
void emit(uint32_t) {}
}  // namespace Manifest

#endif
