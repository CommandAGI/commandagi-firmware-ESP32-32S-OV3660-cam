#include "thermal.h"

#if CAGI_SENSOR_THERMAL

namespace {
bool g_present = false;
}

namespace Sensors {
namespace Thermal {

bool begin() {
  // TODO(hardware): init the IR array over I2C — e.g. for an MLX90640:
  //   Wire.begin(); Wire.setClock(1000000); MLX90640_SetRefreshRate(addr, 0x05 /*16Hz*/);
  //   MLX90640_DumpEE(addr, ee); MLX90640_ExtractParameters(ee, &params);
  // Set g_present from the probe result.
  g_present = false;  // flip true once the real init above succeeds
  Serial.println("[thermal] begin — stub (no driver wired yet); reports absent");
  return g_present;
}

bool present() { return g_present; }

bool read(ThermalFrame& out) {
  // TODO(hardware): MLX90640_GetFrameData + MLX90640_CalculateTo → per-pixel Celsius, scaled to
  // centi-degrees into out.cC[]. Preserve the real temperature distribution (the platform reads scene
  // consistency vs uniform-panel from it) — do not clamp or flatten.
  out.valid = false;
  for (uint16_t i = 0; i < ThermalFrame::kW * ThermalFrame::kH; i++) out.cC[i] = 0;
  return false;  // stub: no frame until the driver above is wired
}

}  // namespace Thermal
}  // namespace Sensors

#endif  // CAGI_SENSOR_THERMAL
