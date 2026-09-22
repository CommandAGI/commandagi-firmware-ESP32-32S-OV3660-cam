#include "tamper.h"

#if CAGI_TAMPER_ENABLED
#include <Preferences.h>
#if CAGI_SECURE_ELEMENT
#include <Wire.h>
#endif

namespace {
bool g_begun = false;
// Cached latch so tamper_case_intact() is a cheap read on the hot path (called per manifest emit).
// false = a breach has been latched (case was opened at some point). Loaded from NVS/SE in begin().
bool g_intact = true;

// Read the persisted latch. On a NON-SE build this is the NVS mirror. On an SE build the AUTHORITATIVE
// source is the SE tamper register; the NVS copy is only a fast mirror we still consult for the case
// where the SE read is unavailable this boot.
bool loadLatchedIntact() {
  bool intact = true;
  Preferences p;
  if (p.begin(CAGI_NVS_NAMESPACE, /*readOnly=*/true)) {
    // Key present AND true means "breach recorded" → not intact.
    if (p.isKey(CAGI_TAMPER_NVS_KEY) && p.getBool(CAGI_TAMPER_NVS_KEY, false)) intact = false;
    p.end();
  }
#if CAGI_SECURE_ELEMENT
  // TODO(hardware): read the ATECC608 tamper/latch register over I2C (CAGI_SE_I2C_ADDR). If the SE
  // reports its tamper input tripped, force intact=false regardless of the NVS mirror — the SE is
  // authoritative and cannot be cleared by wiping NVS. Real read is per-SE-part; wire against the
  // vendor lib during bring-up. Until then the SE build behaves like the NVS latch (fail-open on read).
#endif
  return intact;
}

// Persist a breach irreversibly. Once written, tamper_case_intact() stays false for the unit's life.
void latchBreach() {
  Preferences p;
  if (p.begin(CAGI_NVS_NAMESPACE, /*readOnly=*/false)) {
    p.putBool(CAGI_TAMPER_NVS_KEY, true);
    p.end();
  }
#if CAGI_SECURE_ELEMENT
  // TODO(hardware): latch the breach in the SE's tamper register too, so clearing it requires an SE
  // factory re-attestation (not just a firmware reflash). On parts that support it, this is the write
  // that makes the "case opened" state survive a full NVS wipe.
#endif
}
}  // namespace

namespace Tamper {

void begin() {
  if (g_begun) return;
  pinMode(CAGI_TAMPER_GPIO, INPUT_PULLUP);  // NC loop pulls the pin LOW while the case is shut
#if CAGI_SECURE_ELEMENT
  // TODO(hardware): Wire.begin() on the SE's I2C bus + probe CAGI_SE_I2C_ADDR here if not already up.
#endif
  g_intact = loadLatchedIntact();
  g_begun = true;
  Serial.printf("[tamper] begin — case %s (latched)\n", g_intact ? "intact" : "OPENED");
  poll();  // sample immediately so a break present at boot is caught now
}

void poll() {
  if (!g_begun) return;
  if (!g_intact) return;  // already latched — nothing to do (irreversible)
  // Loop broken (case open) floats the pull-up HIGH. Debounce trivially: a single confirmed HIGH is a
  // breach (the enclosure does not bounce open). A missing/again-closed loop after a real open does NOT
  // clear the latch — that is the whole point of tamper-EVIDENCE.
  if (digitalRead(CAGI_TAMPER_GPIO) == HIGH) {
    g_intact = false;
    latchBreach();
    Serial.println("[tamper] BREACH — case opened; latched (stays tripped short of re-attestation)");
  }
}

bool tamper_case_intact() { return g_intact; }

}  // namespace Tamper

#else  // CAGI_TAMPER_ENABLED == 0 — no tamper switch on this board

namespace Tamper {
void begin() {}
void poll() {}
bool tamper_case_intact() { return true; }  // no switch → never reports a breach
}  // namespace Tamper

#endif
