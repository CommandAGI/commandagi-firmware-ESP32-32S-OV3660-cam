#pragma once
// What a seal's `status` says about the chip's lock (README § Sealed stream, the production lock). Plain
// C++, so test/host/seal_test.cpp seals with it and CommandAGI's verifier checks the result.
//
// The object is canonical JSON (keys sorted, no whitespace): the seal's signature covers the canonical
// form, so a status in any other form would break every seal that carries it.
#include <stddef.h>
#include <stdio.h>

namespace ChipLockStatus {

enum class Flash { Plain, Development, Release };
enum class Download { Open, Secure, Off };

// Each field is what the chip's eFuses (or the build, for NVS) say at runtime, never what a build intends.
struct Facts {
  bool secureBoot;    // the ROM verifies the bootloader, the bootloader verifies the app
  Flash flash;        // flash encryption: off, development (reflashable), release (no plaintext reflash)
  bool nvsEncrypted;  // NVS encryption built in, its keys in an encrypted nvs_keys partition
  bool jtagOff;       // JTAG off on its pins and on USB
  Download download;  // the ROM's serial download mode
};

inline const char* flashWord(Flash f) {
  return f == Flash::Release ? "release" : f == Flash::Development ? "development" : "plain";
}
inline const char* downloadWord(Download d) { return d == Download::Off ? "off" : d == Download::Secure ? "secure" : "open"; }

// {"boot","dl","flash","fw","jtag","nvs"}, in that order. Returns the length, or 0 when `cap` is too small.
inline size_t json(const Facts& f, const char* fw, char* out, size_t cap) {
  const int n = snprintf(out, cap, "{\"boot\":\"%s\",\"dl\":\"%s\",\"flash\":\"%s\",\"fw\":\"%s\",\"jtag\":\"%s\",\"nvs\":\"%s\"}",
                         f.secureBoot ? "verified" : "unverified", downloadWord(f.download), flashWord(f.flash), fw,
                         f.jtagOff ? "off" : "on", f.nvsEncrypted ? "encrypted" : "plain");
  return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

}  // namespace ChipLockStatus
