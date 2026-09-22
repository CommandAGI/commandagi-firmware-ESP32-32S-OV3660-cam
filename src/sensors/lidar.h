#pragma once
#include <Arduino.h>
#include "../config.h"

// LiDAR / ToF depth sensor HAL (verified SKU, behind CAGI_SENSOR_LIDAR).
//
// WHY IT CORROBORATES: a real 3D scene has depth structure (near/far, occlusion edges); a screen or a
// printed photo of a scene is a FLAT plane at one distance. So a coarse depth frame is a strong anti-
// replay signal — the platform's cross-modal coherence (packages/domain/core/src/capture-coherence.ts) reads
// depth FLATNESS: a near-constant depth field with sharp rectangular borders ⇒ a display ⇒ low coherence.
//
// The device only CAPTURES and forwards the depth frame; it does NOT score it (the manifest merely
// declares "lidar" is active). Scoring is the platform's job on the real samples.
//
// Reference part: an ST VL53L5CX (8x8 zone ToF, I2C) or a scanning solid-state module. The interface
// below is part-agnostic; the .cpp holds a documented stub + honest TODOs for the real driver.
namespace Sensors {
namespace Lidar {

// A coarse depth frame: `w`x`h` cells of distance in millimetres (0 = no return / out of range).
struct DepthFrame {
  static constexpr uint8_t kW = 8;
  static constexpr uint8_t kH = 8;
  uint16_t mm[kW * kH];
  bool valid;  // false if the sensor was absent or the read failed
};

// Probe + configure the sensor over I2C. Returns false if absent (build stays functional without it).
bool begin();

// Whether begin() found a working sensor.
bool present();

// Capture one depth frame. `out` is filled and valid=true on success. Stubbed today (see .cpp).
bool read(DepthFrame& out);

}  // namespace Lidar
}  // namespace Sensors
