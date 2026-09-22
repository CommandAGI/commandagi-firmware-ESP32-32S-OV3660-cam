#include "emi.h"

#if CAGI_SENSOR_EMI

namespace {
bool g_present = false;
}

namespace Sensors {
namespace Emi {

bool begin() {
  // TODO(hardware): configure the RF front-end. For the crude ADC-antenna path:
  //   analogReadResolution(12); adcAttachPin(CAGI_EMI_ADC_PIN); set up an I2S/ADC-DMA sampler at a
  //   fixed rate so read() can FFT a window. For an SDR front-end, bring up its SPI/USB link instead.
  // Set g_present from the probe result.
  g_present = false;  // flip true once the real front-end above is wired
  Serial.println("[emi] begin — stub (no front-end wired yet); reports absent");
  return g_present;
}

bool present() { return g_present; }

bool read(EmiSpectrum& out) {
  // TODO(hardware): capture a sample window, run a real FFT (e.g. arduinoFFT), fold |X(f)|^2 into
  // out.mag[] with the fixed bin→frequency mapping the platform expects. Preserve peaks at display-
  // refresh harmonics — that concentration is exactly the replay signal.
  out.valid = false;
  for (uint8_t i = 0; i < EmiSpectrum::kBins; i++) out.mag[i] = 0;
  return false;  // stub: no spectrum until the front-end above is wired
}

}  // namespace Emi
}  // namespace Sensors

#endif  // CAGI_SENSOR_EMI
