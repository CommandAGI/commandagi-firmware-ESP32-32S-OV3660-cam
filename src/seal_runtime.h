#pragma once
#include <Arduino.h>
#include "config.h"
#include "store.h"

// The device seals its own camera stream (build flag CAGI_DEVICE_SEALS; README § Sealed stream). The
// key is an Ed25519 key the device makes on its first boot and keeps in NVS (namespace `cagi-seal`,
// which a factory-reset does not erase). The sealed stream itself is src/device_seal.cpp.
namespace Seal {

#if CAGI_DEVICE_SEALS
// Load the key, or make it on the first boot; take the next block of seqs and counters. Call after
// BleProv::begin(): the radio is on, so esp_fill_random() gives true random numbers.
void begin();
// Provisioning commands on the serial port (README § Sealed stream). Call every loop, with or without
// creds.
void serial();
// Stamp a captured JPEG, send it on the realtime socket, and index it. False: it was not sent.
bool sendFrame(const uint8_t* jpeg, size_t len);
// Once a second: seal what is pending, and send the index and seal lines. Every 10 s: fetch a recent
// block. Call every loop while registered.
void loop(const Creds& c);
// The key's SubjectPublicKeyInfo, base64url (empty when there is no key). BLE INFO carries it.
String spki();
// A text message from the socket. A `seal_resume` (the recorder did not keep what was sent, or the
// socket is new) resets both streams to where the recorder's files stand and answers `seal_resumed`,
// before any later frame goes out. True when the message was one.
bool onMessage(const uint8_t* payload, size_t len);
#endif

}  // namespace Seal
