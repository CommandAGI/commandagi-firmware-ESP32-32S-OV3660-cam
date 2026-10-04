#include "battery.h"
#include "config.h"

namespace Battery {

void begin() {
#if CAGI_BATTERY_ADC_PIN >= 0
  analogSetPinAttenuation(CAGI_BATTERY_ADC_PIN, ADC_11db);  // the divided cell is up to 2.1 V
#endif
#if CAGI_BATTERY_STAT_PIN >= 0
  pinMode(CAGI_BATTERY_STAT_PIN, INPUT_PULLUP);  // STAT outputs are open-drain
#endif
}

Reading read() {
  Reading r;
#if CAGI_BATTERY_ADC_PIN >= 0
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(CAGI_BATTERY_ADC_PIN);
  r.mv = (int)((double)sum / 16.0 * CAGI_BATTERY_DIVIDER);
  // The no-battery variant fits only the lower resistor, so it reads ~0 V.
  r.present = r.mv > 2500;
  if (r.present) {
    // Resting voltage → charge, a typical 1S Li-ion curve. Under load or on charge it reads high or
    // low; it is an estimate, not a fuel gauge.
    static const int kMv[] = {3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200};
    static const int kPct[] = {0, 5, 10, 20, 30, 40, 50, 60, 75, 90, 100};
    if (r.mv <= kMv[0]) r.pct = 0;
    else if (r.mv >= kMv[10]) r.pct = 100;
    else
      for (int i = 1; i < 11; i++)
        if (r.mv <= kMv[i]) {
          r.pct = kPct[i - 1] + (kPct[i] - kPct[i - 1]) * (r.mv - kMv[i - 1]) / (kMv[i] - kMv[i - 1]);
          break;
        }
  }
#endif
#if CAGI_BATTERY_STAT_PIN >= 0
  r.charging = digitalRead(CAGI_BATTERY_STAT_PIN) == CAGI_BATTERY_STAT_ACTIVE ? 1 : 0;
#endif
  return r;
}

}  // namespace Battery
