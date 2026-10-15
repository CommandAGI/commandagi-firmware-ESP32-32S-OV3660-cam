#pragma once
#include <Arduino.h>
#include "store.h"

// Firmware updates on the realtime socket (README § Firmware updates). The host (CommandAGI's
// local-host/drivers/host-wire-firmware.mjs) sends `firmware_report`, `firmware_update { sha256, size, version, url }`
// and `firmware_rollback`; the camera answers `firmware_state` and `firmware_reply`. An update is fetched from the
// host into the other OTA slot, hashed as it arrives, refused when its SHA-256 or size differs, and booted. The new
// image confirms itself only once its socket reaches the host again; one that does not within CAGI_OTA_CONFIRM_MS
// (or resets first) is taken back by the bootloader (ESP-IDF app rollback).
namespace Ota {
// At boot, before the loop: the boot count, and what became of an update written before this boot.
void begin();
// One text message from the socket. True when it was a firmware_* message (handled here).
bool onMessage(const uint8_t* payload, size_t len);
// The socket reached the host: a new image that waits for that confirms itself.
void onConnected();
// From the loop, after the socket's loop: answer a report, run a queued update or rollback (blocking; it restarts the
// device when it boots another slot).
void loop(const Creds& c);
// From the top of the loop, with or without a link: take back a new image that did not reach the host in time.
void watch();
}  // namespace Ota
