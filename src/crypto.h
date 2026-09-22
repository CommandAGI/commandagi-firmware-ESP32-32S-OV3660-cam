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

// ── Ed25519 capture-manifest signing (verified SKU) ──────────────────────────────────────────────
// Mirrors packages/domain/core/src/vc.ts + integrity.ts: a manifest's signature is Ed25519 over the RAW bytes
// of the domain-separated message string ("cagi-manifest:v1|...") — the TS side hex-encodes the UTF-8
// message and signs those decoded bytes, which is exactly the UTF-8 message bytes, so signing the
// message bytes here produces the same signature the platform's verifyManifest() checks.
//
// Two key custodies, selected by CAGI_SECURE_ELEMENT:
//   - 0 (device_key tier): the 32-byte Ed25519 seed lives in NVS; we sign in software.
//   - 1 (secure_element tier): the private key is generated inside and never leaves the secure element —
//     we hand the message to the SE and it returns the signature. The seed argument is ignored.
// PART CONSTRAINT: the SE MUST produce **Ed25519** (the scheme integrity.ts verifies). The common
// ATECC608 is **ECDSA P-256 only** and CANNOT be used here — use an Ed25519-capable SE (NXP SE050 is the
// reference; see hardware/VERIFIED_SKU.md §3). Returns lowercase hex like the TS helpers (64-byte sig →
// 128 hex chars; 32-byte pubkey → 64 hex).

// Ed25519 public key (hex) for the manifest signer. On the SE build this reads the SE's public key; on
// the NVS build it derives it from `seed32`.
String ed25519PubKeyHex(const uint8_t seed32[32]);

// Sign `len` bytes at `msg` and return the signature as 128 hex chars. On the SE build `seed32` is
// ignored (the SE holds the key). Returns "" on failure.
String ed25519SignHex(const uint8_t* msg, size_t len, const uint8_t seed32[32]);

}  // namespace Crypto
