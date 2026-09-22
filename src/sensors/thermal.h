#pragma once
#include <Arduino.h>
#include "../config.h"

// Thermal / long-wave IR array HAL (verified SKU, behind CAGI_SENSOR_THERMAL). Modality tag: "ir".
//
// WHY IT CORROBORATES: a real scene has a temperature DISTRIBUTION — warm bodies, cool walls, hot
// surfaces — while an emissive display is a near-UNIFORM panel temperature across its whole face. So a
// low-res thermal frame whose temperatures are nearly constant (and whose warm region is a perfect
// rectangle) is a display tell. The platform's coherence check reads thermal scene-consistency from
// this frame; the device only forwards it and declares "ir" active.
//
// Reference part: a Melexis MLX90640 (32x24 IR array, I2C). Interface is part-agnostic; the .cpp is a
// documented stub with honest TODOs.
namespace Sensors {
namespace Thermal {

// A coarse thermal frame: `w`x`h` cells of temperature in centi-degrees Celsius (e.g. 2350 = 23.50 C).
struct ThermalFrame {
  static constexpr uint8_t kW = 32;
  static constexpr uint8_t kH = 24;
  int16_t cC[kW * kH];  // centi-degrees Celsius
  bool valid;
};

bool begin();
bool present();
bool read(ThermalFrame& out);

}  // namespace Thermal
}  // namespace Sensors
