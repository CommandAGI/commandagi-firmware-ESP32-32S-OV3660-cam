#pragma once
// The IR night-mode decision. Plain C++ with no Arduino or ESP-IDF headers, so
// test/host/ir_policy_test.cpp compiles it with g++ on a computer.
#include <string.h>

namespace IrPolicy {

enum class Mode : unsigned char { Auto, On, Off };

// "auto" | "on" | "off". Anything else returns false and leaves *out unchanged.
inline bool parseMode(const char* s, Mode* out) {
  if (!s) return false;
  if (!strcmp(s, "auto")) { *out = Mode::Auto; return true; }
  if (!strcmp(s, "on")) { *out = Mode::On; return true; }
  if (!strcmp(s, "off")) { *out = Mode::Off; return true; }
  return false;
}

inline const char* modeName(Mode m) {
  return m == Mode::On ? "on" : (m == Mode::Off ? "off" : "auto");
}

struct Thresholds {
  float onBelowLux;   // auto: IR goes on below this
  float offAboveLux;  // auto: IR goes off above this (> onBelowLux: the band between is hysteresis)
};

// Whether the IR LEDs are on after this step.
// cameraRunning: the camera is wired, initialised and the operator wants it on. If not, IR is off in
//   every mode: IR light with no camera to use it is only heat and current.
// lux: the latest ambient light reading; < 0 = no reading. In auto mode no reading means off, because
//   the firmware does not know that it is dark.
// irWasOn: the state after the previous step (the hysteresis memory).
inline bool decide(Mode mode, bool cameraRunning, float lux, bool irWasOn, Thresholds t) {
  if (!cameraRunning) return false;
  if (mode == Mode::On) return true;
  if (mode == Mode::Off) return false;
  if (lux < 0) return false;
  return irWasOn ? !(lux > t.offAboveLux) : lux < t.onBelowLux;
}

}  // namespace IrPolicy
