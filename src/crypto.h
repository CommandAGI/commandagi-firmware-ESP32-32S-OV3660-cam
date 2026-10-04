#pragma once
#include <Arduino.h>

// PIN-keyed AEAD for the provisioning payload. Mirrors packages/domain/core/src/esp32cam.ts:
//   key  = HKDF-SHA256(ikm = PIN, salt, info = "cagi-cam-prov-v1", 32 bytes)
//   wire = IV(12) || AES-256-GCM(key, IV, plaintext) || tag(16)
// The PIN is an out-of-band secret (printed on the device) — a BLE sniffer without it learns nothing.
namespace Crypto {
// Fill `out` with `len` cryptographically-random bytes (esp_fill_random).
void randomBytes(uint8_t* out, size_t len);
// Lowercase hex of a buffer.
String toHex(const uint8_t* buf, size_t len);
// Decrypt a sealed provisioning blob. Returns true and fills `outJson` on success; false if the blob
// is malformed or the PIN is wrong (GCM tag mismatch).
bool openSealed(const uint8_t* wire, size_t wireLen, const char* pin, const uint8_t* salt, size_t saltLen,
                String& outJson);

}  // namespace Crypto
