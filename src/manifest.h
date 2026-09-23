#pragma once
#include <Arduino.h>
#include "config.h"

// Signed capture manifest (verified SKU, behind CAGI_VERIFIED_SKU) — the device's SELF-DESCRIPTION that
// rides the stream as a sidecar alongside frames. Mirrors packages/domain/core/src/integrity.ts CaptureManifest:
//   { seq, fw, bootState, caseIntact, modalities[], selfCoherence, signer, signature }
// signed Ed25519 over "cagi-manifest:v1|seq|fw|bootState|caseIntact(1/0)|modalities.join(',')|selfCoherence".
//
// IT IS EVIDENCE, NOT AUTHORITY. The device declares what it is (firmware, boot state, case-intact switch,
// which sensors are live) and signs it so the claim cannot be forged in transit — but the platform
// RE-VERIFIES the signature, RECOMPUTES cross-modal coherence from the actual frames, and reads the
// modality set from the real streams. selfCoherence here is only a hint; the device never gets to set
// its own integrity score. See docs/architecture/trust/INTEGRITY.md §6.
namespace Manifest {

// Load the manifest signer key custody. On a non-SE build this reads the device Ed25519 seed from NVS
// (persisted at provisioning time from DeviceProvisioning.devicePrivKeyHex); on an SE build the key
// lives in the secure element and this just readies the SE handle. Idempotent.
void begin();

// Build the current manifest for chain segment `seq`, sign it, and emit it over the frame socket as a
// JSON sidecar. No-op if signing is unavailable (no key / SE not ready) — a missing manifest simply
// means the platform scores the source without the multi-modal lift, never that it fails open.
void emit(uint32_t seq);

}  // namespace Manifest
