#pragma once
// The chip's lock as the chip reports it, and the factory's serial commands that need no key (README
// § Sealed stream, the production lock). A production build (CommandAGI-Cam-002's `-production` envs) is
// one signed image for every unit, so its provisioning PIN cannot be compiled in: the factory stores it
// in NVS once (`prov-pin`), and NVS encryption keeps it with the sealing key.
#include "chip_lock_status.h"
#include "config.h"

#if CAGI_PROV_PIN_NVS && !CAGI_DEVICE_SEALS
#error "CAGI_PROV_PIN_NVS needs CAGI_DEVICE_SEALS: the factory's serial commands run in the sealing build"
#endif

namespace ChipLock {

// Read from the eFuses, the flash encryption state and the partition table on each call.
ChipLockStatus::Facts facts();
// The PIN that keys BLE provisioning: compiled in (CAGI_PROV_PIN), or from NVS in a CAGI_PROV_PIN_NVS build.
// Null when a CAGI_PROV_PIN_NVS unit has none yet; such a unit refuses every PROVISION write.
const char* provPin();
// A serial line for this module (`prov-pin <6 digits>`). True when it was one, answered or refused.
bool command(const char* line);

}  // namespace ChipLock
