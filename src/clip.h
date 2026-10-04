#pragma once
// Decode one speaker clip (WAV PCM16 or MP3) into mono PCM16. Plain C++ with no Arduino or ESP-IDF
// headers, so test/host/clip_test.cpp compiles it with g++ on a computer. The clip comes from the
// network: every length and field is checked before it is used.
#include <stddef.h>
#include <stdint.h>

namespace Clip {

// Where the decoded PCM goes. The device allocates in PSRAM; the host test uses malloc.
struct Allocator {
  void* (*alloc)(size_t bytes);
  void (*free)(void* p);
};

struct Pcm {
  int16_t* samples = nullptr;  // mono, owned by the caller after a successful decode (free with Allocator)
  size_t count = 0;            // number of samples
  uint32_t rate = 0;           // Hz, 8000..48000
  uint32_t durationMs() const { return rate ? (uint32_t)((uint64_t)count * 1000u / rate) : 0; }
};

// Sniff the bytes (and the optional `format`/`mime` hint from the command), decode, and check the
// bounds. On failure returns false, frees anything it allocated and sets *error to a short reason.
//   maxBytes : refuse a source larger than this
//   maxMs    : refuse a clip longer than this
bool decode(const uint8_t* data, size_t len, const char* hint, size_t maxBytes, uint32_t maxMs,
            const Allocator& a, Pcm* out, const char** error);

// Standard base64 (RFC 4648, with or without padding; whitespace and the JSON escape "\/" are
// skipped). Writes at most `cap` bytes. Returns false on a bad character or if the result is larger
// than `cap`.
bool base64Decode(const char* in, size_t inLen, uint8_t* out, size_t cap, size_t* outLen);

}  // namespace Clip
