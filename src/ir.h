#pragma once
// IR night mode (-DCAGI_IR_ENABLED=1): the IR LEDs' enable pin, the ambient light sensor and the
// camera's night tuning. README § IR night mode.
#include <Arduino.h>
#include "ir_policy.h"

namespace Ir {

struct State {
  IrPolicy::Mode mode = IrPolicy::Mode::Auto;  // the operator's desired mode
  bool on = false;      // the level the firmware drives on the enable pin. Not proof that the LEDs emit.
  float lux = -1;       // the latest light reading; < 0 = none
  bool night = false;   // the camera's night tuning is set (the sensor accepted the writes)
};

// Drive the enable pin low. Call first in setup(): the pin floats until then.
void begin();
void setMode(IrPolicy::Mode mode);
// One step: read the light sensor every CAGI_IR_READ_MS, decide, drive the pin, set the tuning.
// cameraRunning: the camera is initialised and the operator wants it on. busUp: the camera driver
// holds the I2C port the light sensor shares (the sensor sits on the camera's SCCB lines).
// Returns true when mode, on or night changed.
bool service(uint32_t now, bool cameraRunning, bool busUp);
// The camera driver was re-initialised: its registers, and so the night tuning, are back to default.
void cameraReset();
State state();

}  // namespace Ir
