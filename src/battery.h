#pragma once
#include <Arduino.h>

// Battery state from an ADC divider (-DCAGI_BATTERY_ADC_PIN) and an optional charger STAT pin.
// Without CAGI_BATTERY_ADC_PIN, present() is false and nothing is reported.
namespace Battery {

struct Reading {
  bool present = false;   // a cell is connected (the divider reads above 2.5 V)
  int mv = 0;             // cell voltage, mV
  int pct = -1;           // estimate from the resting Li-ion voltage curve; -1 = unknown
  int charging = -1;      // 1 / 0 from the STAT pin; -1 = no STAT pin
};

void begin();
// Sample now (16 ADC reads, ~2 ms).
Reading read();

}  // namespace Battery
