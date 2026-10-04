#include "config.h"
#if CAGI_IR_ENABLED
#include "ir.h"
#include "esp_camera.h"
#include "driver/i2c.h"

// The LTR-308 shares GPIO8/9 with the camera's SCCB. The camera driver installs the legacy I2C
// driver on its SCCB port; a second controller (Arduino's Wire on port 0) on the same pins would
// fight it, so the light sensor uses that same port. The driver serialises each transaction.
#if CONFIG_SCCB_HARDWARE_I2C_PORT1
#define IR_I2C_PORT I2C_NUM_1
#else
#define IR_I2C_PORT I2C_NUM_0
#endif

namespace {
// LTR-308ALS-01 registers (Lite-On datasheet; DFRobot_LTR308 uses the same).
constexpr uint8_t kAddr = CAGI_ALS_I2C_ADDR;
constexpr uint8_t kRegCtrl = 0x00, kRegMeasRate = 0x04, kRegGain = 0x05, kRegPartId = 0x06, kRegData0 = 0x0D;
constexpr uint8_t kPartId = 0xB1;
constexpr uint8_t kCtrlAlsOn = 0x02;
constexpr uint8_t kGain3x = 0x01;
constexpr uint8_t kMeas100ms18bit500ms = (0x02 << 4) | 0x03;  // 100 ms conversion, one every 500 ms
// lux = 0.6 * count / (gain * integration/100 ms) = count * 0.2 at gain 3, 100 ms.
constexpr float kLuxPerCount = 0.6f / 3.0f;

const IrPolicy::Thresholds kThresholds = {CAGI_IR_ON_BELOW_LUX, CAGI_IR_OFF_ABOVE_LUX};

Ir::State g_state;
bool g_alsReady = false;
uint32_t g_nextRead = 0;
int8_t g_tuning = -1;  // the night tuning written to the sensor: 1 night, 0 day, -1 unknown

bool alsWrite(uint8_t reg, uint8_t v) {
  const uint8_t b[2] = {reg, v};
  return i2c_master_write_to_device(IR_I2C_PORT, kAddr, b, 2, pdMS_TO_TICKS(20)) == ESP_OK;
}

bool alsRead(uint8_t reg, uint8_t* out, size_t n) {
  return i2c_master_write_read_device(IR_I2C_PORT, kAddr, &reg, 1, out, n, pdMS_TO_TICKS(20)) == ESP_OK;
}

bool alsStart() {
  uint8_t id = 0;
  if (!alsRead(kRegPartId, &id, 1) || id != kPartId) return false;
  return alsWrite(kRegGain, kGain3x) && alsWrite(kRegMeasRate, kMeas100ms18bit500ms) && alsWrite(kRegCtrl, kCtrlAlsOn);
}

float alsLux() {
  uint8_t d[3];
  if (!alsRead(kRegData0, d, 3)) return -1;
  const uint32_t count = ((uint32_t)(d[2] & 0x0F) << 16) | ((uint32_t)d[1] << 8) | d[0];
  return count * kLuxPerCount;
}

// OV3660: set_aec2 is the sensor's night mode (register 0x3A00 bit 2: longer exposure at low light).
// The image is grey while IR is on: the module's lens passes 940 nm, so colours under IR are false.
bool setTuning(bool night) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s || !s->set_aec2 || !s->set_special_effect) return false;
  return s->set_aec2(s, night ? 1 : 0) == 0 && s->set_special_effect(s, night ? 2 : 0) == 0;
}
}  // namespace

namespace Ir {

void begin() {
  pinMode(CAGI_IR_PIN, OUTPUT);
  digitalWrite(CAGI_IR_PIN, LOW);
}

void setMode(IrPolicy::Mode mode) {
  if (mode != g_state.mode) Serial.printf("[ir] mode %s\n", IrPolicy::modeName(mode));
  g_state.mode = mode;
}

bool service(uint32_t now, bool cameraRunning, bool busUp) {
  const State before = g_state;
  bool tick = false;
  if (!busUp) {
    g_alsReady = false;
    g_state.lux = -1;
  } else if ((int32_t)(now - g_nextRead) >= 0) {
    g_nextRead = now + CAGI_IR_READ_MS;
    tick = true;
    if (!g_alsReady) {
      g_alsReady = alsStart();
      Serial.printf("[ir] light sensor LTR-308 %s\n", g_alsReady ? "ready" : "not found");
    } else {
      g_state.lux = alsLux();
      if (g_state.lux < 0) g_alsReady = false;  // start it again on the next read
    }
  }

  g_state.on = IrPolicy::decide(g_state.mode, cameraRunning, g_state.lux, g_state.on, kThresholds);
  digitalWrite(CAGI_IR_PIN, g_state.on ? HIGH : LOW);

  // A failed write is tried again on the next read, not on every pass of the loop.
  if (busUp && g_tuning != (g_state.on ? 1 : 0) && (tick || before.on != g_state.on)) {
    g_tuning = setTuning(g_state.on) ? (g_state.on ? 1 : 0) : -1;
  }
  if (!busUp) g_tuning = -1;
  g_state.night = g_tuning == 1;

  const bool changed = before.mode != g_state.mode || before.on != g_state.on || before.night != g_state.night;
  if (before.on != g_state.on) {
    if (g_state.lux >= 0) Serial.printf("[ir] %s (mode %s, %.1f lx)\n", g_state.on ? "on" : "off", IrPolicy::modeName(g_state.mode), g_state.lux);
    else Serial.printf("[ir] %s (mode %s, no light reading)\n", g_state.on ? "on" : "off", IrPolicy::modeName(g_state.mode));
  }
  return changed;
}

void cameraReset() { g_tuning = -1; }

State state() { return g_state; }

}  // namespace Ir
#endif
