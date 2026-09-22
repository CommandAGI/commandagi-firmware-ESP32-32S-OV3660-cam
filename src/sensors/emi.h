#pragma once
#include <Arduino.h>
#include "../config.h"

// EMI / RF near-field probe HAL (verified SKU, behind CAGI_SENSOR_EMI). Modality tag: "emi".
//
// WHY IT CORROBORATES: an emissive display (LCD/OLED) radiates a characteristic near-field EM signature
// dominated by its refresh rate and pixel-clock harmonics (~50-240 Hz refresh + kHz/MHz line/pixel
// clocks). A real outdoor/indoor scene in front of the camera does NOT. So energy concentrated at
// display-refresh frequencies in the near-field spectrum is a strong "a screen is present" replay tell.
// The platform's coherence check reads this display-EMI signature; the device only forwards a coarse
// spectrum and declares "emi" active.
//
// Reference front-end: a short whip/loop antenna into the ESP32 ADC (crude) or a small SDR receiver for
// a real spectrum. Interface is front-end-agnostic; the .cpp is a documented stub with honest TODOs.
namespace Sensors {
namespace Emi {

// A coarse EMI power spectrum: `bins` magnitude buckets over a fixed near-field band. The bin→frequency
// mapping is fixed by the front-end config so the platform can look for display-refresh harmonics.
struct EmiSpectrum {
  static constexpr uint8_t kBins = 64;
  uint16_t mag[kBins];  // relative magnitude per bin (arbitrary units, per-device normalised)
  bool valid;
};

bool begin();
bool present();
bool read(EmiSpectrum& out);

}  // namespace Emi
}  // namespace Sensors
