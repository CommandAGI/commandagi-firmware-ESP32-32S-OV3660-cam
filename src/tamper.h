#pragma once
#include <Arduino.h>
#include "config.h"

// Chassis tamper switch (verified SKU only) — a normally-closed loop across the enclosure seam.
//
// TAMPER-EVIDENT, NOT TAMPER-PROOF. This detects and RECORDS that the case was opened; it cannot stop a
// determined attacker who has physical possession. The honest security model is:
//   - The switch reads on a GPIO with an internal pull-up: case shut = loop closed = LOW; case opened =
//     loop broken = HIGH. The first HIGH reading LATCHES an irreversible "case was opened" flag.
//   - On a NON-secure-element build the latch lives in NVS. This is best-effort: an attacker who can
//     reflash or wipe NVS can clear it. It still raises the cost of an undetected swap and gives the
//     platform a signal (the manifest's caseIntact goes false and stays false).
//   - On a CAGI_SECURE_ELEMENT build the AUTHORITATIVE latch is the secure element's own tamper input:
//     tripping it zeroizes / locks the SE's protected key, so a source whose case was opened can no
//     longer produce valid signatures at all — clearing the latch requires a factory SE re-attestation.
//     That is what makes the device-tier "the key never left silicon" claim physically enforceable
//     rather than merely asserted; see TAMPER_DISCOUNT in packages/domain/core/src/integrity.ts.
//
// The whole module is compiled out unless CAGI_TAMPER_ENABLED. When disabled, tamper_case_intact()
// returns true (a board with no tamper switch simply never reports a breach).
namespace Tamper {

// Configure the GPIO (INPUT_PULLUP) and, on the SE variant, bind the SE tamper register. Reads the
// latched state from NVS/SE so a case opened while powered off is still remembered. Idempotent.
void begin();

// Sample the switch once and, if the loop is broken, LATCH the breach (NVS + SE). Call periodically
// from loop() so an opening during operation is caught, not just at boot. Cheap (one digitalRead).
void poll();

// True iff the case has NEVER been recorded as opened (the value that rides the signed CaptureManifest
// as `caseIntact`). Once the latch trips this stays false for the life of the unit, short of a factory
// / SE re-attestation. Returns true unconditionally when CAGI_TAMPER_ENABLED is 0.
bool tamper_case_intact();

}  // namespace Tamper
