#include "lidar.h"

#if CAGI_SENSOR_LIDAR

namespace {
bool g_present = false;
}

namespace Sensors {
namespace Lidar {

bool begin() {
  // TODO(hardware): bring up the ToF part over I2C — e.g. for a VL53L5CX:
  //   Wire.begin(); vl53l5cx_init(&dev); vl53l5cx_set_resolution(&dev, 64);
  //   vl53l5cx_set_ranging_frequency_hz(&dev, 15); vl53l5cx_start_ranging(&dev);
  // Set g_present from the init result. Kept as a clean stub so the verified build links and the
  // manifest can declare "lidar" without blocking on a specific part being wired.
  g_present = false;  // flip true once the real init above succeeds
  Serial.println("[lidar] begin — stub (no driver wired yet); reports absent");
  return g_present;
}

bool present() { return g_present; }

bool read(DepthFrame& out) {
  // TODO(hardware): poll vl53l5cx_check_data_ready + vl53l5cx_get_ranging_data and copy the per-zone
  // distance_mm[] into out.mm[]. The platform reads depth FLATNESS from this, so preserve real spatial
  // variance — do not smooth or normalise here.
  out.valid = false;
  for (uint16_t i = 0; i < DepthFrame::kW * DepthFrame::kH; i++) out.mm[i] = 0;
  return false;  // stub: no frame until the driver above is wired
}

}  // namespace Lidar
}  // namespace Sensors

#endif  // CAGI_SENSOR_LIDAR
