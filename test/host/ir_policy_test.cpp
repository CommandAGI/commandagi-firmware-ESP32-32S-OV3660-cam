// Host test of the IR night-mode decision (src/ir_policy.h): a light level that drifts across the
// thresholds must not make the LEDs flicker, and IR must be off whenever the camera does not run.
// Run from the repo root:
//
//   g++ -std=c++17 -O1 -Wall -Isrc test/host/ir_policy_test.cpp -o /tmp/ir_policy_test && /tmp/ir_policy_test
#include <cstdio>
#include <initializer_list>
#include "ir_policy.h"

using IrPolicy::Mode;

namespace {
int g_failed = 0;
const IrPolicy::Thresholds kT = {5.0f, 15.0f};

void expect(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL %s\n", what); g_failed++; }
}

// Feed lux readings in auto mode with the camera running; return the IR state after each one.
void run(const float* lux, int n, bool* out) {
  bool on = false;
  for (int i = 0; i < n; i++) out[i] = on = IrPolicy::decide(Mode::Auto, true, lux[i], on, kT);
}
}  // namespace

int main() {
  // Dusk, noise around both thresholds, dawn. Between 5 and 15 lx the state does not change.
  const float lux[] = {100, 20, 14, 6, 5, 4.9f, 6, 12, 15, 14, 15.1f, 10, 5.1f, 4, -1, 3};
  const bool want[] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1};
  const int n = sizeof(lux) / sizeof(lux[0]);
  bool got[n];
  run(lux, n, got);
  for (int i = 0; i < n; i++) {
    if (got[i] != want[i]) {
      std::printf("FAIL auto step %d: lux %.1f → %s, want %s\n", i, lux[i], got[i] ? "on" : "off", want[i] ? "on" : "off");
      g_failed++;
    }
  }

  // The camera does not run (paused, off, failed): IR is off in every mode, at any light, from any state.
  for (Mode m : {Mode::Auto, Mode::On, Mode::Off})
    for (bool was : {false, true})
      expect(!IrPolicy::decide(m, false, 0.0f, was, kT), "IR off while the camera does not run");

  // The operator's on/off override the light; off holds in the dark.
  expect(IrPolicy::decide(Mode::On, true, 1000.0f, false, kT), "on in daylight");
  expect(!IrPolicy::decide(Mode::Off, true, 0.0f, true, kT), "off in the dark");

  // The wire values.
  Mode m = Mode::On;
  expect(IrPolicy::parseMode("off", &m) && m == Mode::Off, "parse off");
  expect(!IrPolicy::parseMode("OFF", &m) && m == Mode::Off, "an unknown value leaves the mode");
  expect(!IrPolicy::parseMode(nullptr, &m), "parse null");

  if (g_failed) { std::printf("%d failed\n", g_failed); return 1; }
  std::printf("ir_policy_test: ok\n");
  return 0;
}
