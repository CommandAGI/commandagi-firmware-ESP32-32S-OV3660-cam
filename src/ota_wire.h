#pragma once
// The pure parts of a firmware update on the realtime socket (plain C++, host-tested in test/host/ota_wire_test.cpp):
// what an update message must say, where the image is fetched from, whether the bytes that came are the image it
// named, and what a slot's kept digest is worth. The ESP32 side is src/ota.cpp; the wire is CommandAGI's
// packages/runtime/local-host/drivers/host-wire-firmware.mjs.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace OtaWire {

// 64 lowercase hex characters, nothing else.
inline bool isSha256Hex(const char* s) {
  if (!s || strlen(s) != 64) return false;
  for (int i = 0; i < 64; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
  return true;
}

inline void toHex(const uint8_t digest[32], char out[65]) {
  static const char k[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[2 * i] = k[digest[i] >> 4];
    out[2 * i + 1] = k[digest[i] & 15];
  }
  out[64] = 0;
}

// Why an update cannot be taken as it is, or nullptr. `path` is where the host serves the image, on the listener the
// device dialed: /v1/firmware/<session>/<device>/<sha256>, ending with the digest the update names.
inline const char* checkUpdate(const char* sha256, long long size, const char* path, size_t slotSize) {
  if (!isSha256Hex(sha256)) return "the update names no sha256 (64 lowercase hex)";
  if (size <= 0) return "the update names no size";
  if ((unsigned long long)size > slotSize) return "the image is larger than the OTA slot";
  if (!path || strncmp(path, "/v1/firmware/", 13) != 0) return "the image url is not a /v1/firmware/ path on the host";
  if (strstr(path, "..") || strchr(path, '?') || strchr(path, '#') || strlen(path) > 240) return "the image url is not a plain path";
  const size_t n = strlen(path);
  if (n < 65 || path[n - 65] != '/' || strcmp(path + n - 64, sha256) != 0) return "the image url does not end with the digest the update names";
  return nullptr;
}

// The origin of `base` (scheme://host[:port], as provisioned) followed by `path`. False when it does not fit or the
// base is not http(s).
inline bool joinUrl(const char* base, const char* path, char* out, size_t cap) {
  size_t skip;
  if (!strncmp(base, "https://", 8)) skip = 8;
  else if (!strncmp(base, "http://", 7)) skip = 7;
  else return false;
  const size_t origin = skip + strcspn(base + skip, "/?#");
  if (origin == skip || origin + strlen(path) + 1 > cap) return false;
  memcpy(out, base, origin);
  strcpy(out + origin, path);
  return true;
}

// Bytes as they come: refuses more than the update stated.
struct Receive {
  long long expected = 0;
  long long got = 0;
  bool over = false;
  bool take(size_t n) {
    got += (long long)n;
    if (got > expected) over = true;
    return !over;
  }
};

enum class Verdict { Ok, Size, Digest };
inline Verdict verdict(const Receive& r, const char* wantSha, const char* gotSha) {
  if (r.over || r.got != r.expected) return Verdict::Size;
  return strcmp(wantSha, gotSha) == 0 ? Verdict::Ok : Verdict::Digest;
}

// What the device keeps per slot (NVS): the SHA-256 of the image FILE it wrote there, and the partition digest the
// ESP-IDF computed over that slot right after (esp_partition_get_sha256). The file digest still holds only while the
// slot's partition digest is the one kept: a slot flashed again over USB has another one, and its file is unknown.
struct SlotRecord {
  char fileSha[65] = "";
  char partitionSha[65] = "";
  char version[32] = "";
};
inline const char* fileShaOf(const SlotRecord& r, const char* partitionShaNow) {
  if (!isSha256Hex(r.fileSha) || !isSha256Hex(r.partitionSha) || !partitionShaNow) return nullptr;
  return strcmp(r.partitionSha, partitionShaNow) == 0 ? r.fileSha : nullptr;
}

// After a boot, what became of the update written to `pendingSlot` (empty: none): the device runs it and has yet to
// confirm it, or the bootloader went back to the slot before it.
enum class Boot { Plain, Confirming, WentBack };
inline Boot bootOutcome(const char* pendingSlot, const char* runningSlot) {
  if (!pendingSlot || !*pendingSlot) return Boot::Plain;
  return strcmp(pendingSlot, runningSlot) == 0 ? Boot::Confirming : Boot::WentBack;
}

}  // namespace OtaWire
