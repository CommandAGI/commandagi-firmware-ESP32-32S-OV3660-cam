#include "clip.h"
#include <string.h>

// minimp3 (CC0, vendored from github.com/lieff/minimp3 at ea99364f) — layer III only, no SIMD.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "third_party/minimp3.h"

namespace {

constexpr uint32_t kMinRate = 8000;
constexpr uint32_t kMaxRate = 48000;

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

enum class Kind { Sniff, Wav, Mp3, Other };

// Case-insensitive compare of the hint's media part (before any ';') with one name.
bool hintIs(const char* hint, size_t n, const char* name) {
  size_t m = strlen(name);
  if (n != m) return false;
  for (size_t i = 0; i < n; i++) {
    char c = hint[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != name[i]) return false;
  }
  return true;
}

Kind kindOf(const char* hint) {
  if (!hint) return Kind::Sniff;
  while (*hint == ' ') hint++;
  size_t n = 0;
  while (hint[n] && hint[n] != ';' && hint[n] != ' ') n++;
  if (n == 0) return Kind::Sniff;
  static const char* const wav[] = {"wav", "wave", "audio/wav", "audio/x-wav", "audio/wave", "audio/vnd.wave"};
  static const char* const mp3[] = {"mp3", "mpeg", "audio/mpeg", "audio/mp3", "audio/mpeg3"};
  for (const char* w : wav) if (hintIs(hint, n, w)) return Kind::Wav;
  for (const char* m : mp3) if (hintIs(hint, n, m)) return Kind::Mp3;
  return Kind::Other;
}

bool isRiffWave(const uint8_t* d, size_t len) {
  return len >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0;
}

bool fail(const char** error, const char* why) {
  if (error) *error = why;
  return false;
}

bool decodeWav(const uint8_t* d, size_t len, uint32_t maxMs, const Clip::Allocator& a, Clip::Pcm* out,
               const char** error) {
  size_t pos = 12;
  bool haveFmt = false;
  uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
  uint32_t rate = 0;
  const uint8_t* pcm = nullptr;
  size_t pcmBytes = 0;
  while (pos + 8 <= len) {
    const uint8_t* ck = d + pos;
    const uint32_t size = rd32(ck + 4);
    const size_t body = pos + 8;
    if (size > len - body) {
      // Only the data chunk may be cut short by a streaming writer; any other chunk must fit.
      if (memcmp(ck, "data", 4) != 0) return fail(error, "wav: chunk runs past the end");
      return fail(error, "wav: data chunk runs past the end");
    }
    if (memcmp(ck, "fmt ", 4) == 0) {
      if (size < 16) return fail(error, "wav: fmt chunk too short");
      format = rd16(d + body);
      channels = rd16(d + body + 2);
      rate = rd32(d + body + 4);
      blockAlign = rd16(d + body + 12);
      bits = rd16(d + body + 14);
      haveFmt = true;
    } else if (memcmp(ck, "data", 4) == 0) {
      pcm = d + body;
      pcmBytes = size;
      break;
    }
    pos = body + size + (size & 1);  // chunks are padded to an even length
  }
  if (!haveFmt) return fail(error, "wav: no fmt chunk");
  if (!pcm) return fail(error, "wav: no data chunk");
  if (format != 1) return fail(error, "wav: not PCM");
  if (bits != 16) return fail(error, "wav: not 16-bit");
  if (channels != 1 && channels != 2) return fail(error, "wav: not mono or stereo");
  if (blockAlign != channels * 2) return fail(error, "wav: bad block align");
  if (rate < kMinRate || rate > kMaxRate) return fail(error, "wav: sample rate outside 8-48 kHz");
  const size_t frames = pcmBytes / blockAlign;
  if (frames == 0) return fail(error, "wav: no samples");
  if ((uint64_t)frames * 1000u > (uint64_t)maxMs * rate) return fail(error, "clip longer than the limit");
  int16_t* s = (int16_t*)a.alloc(frames * sizeof(int16_t));
  if (!s) return fail(error, "out of memory");
  for (size_t i = 0; i < frames; i++) {
    const uint8_t* f = pcm + i * blockAlign;
    if (channels == 1) s[i] = (int16_t)rd16(f);
    else s[i] = (int16_t)(((int32_t)(int16_t)rd16(f) + (int32_t)(int16_t)rd16(f + 2)) / 2);
  }
  out->samples = s;
  out->count = frames;
  out->rate = rate;
  return true;
}

// Skip an ID3v2 tag at the start (its size is a 28-bit syncsafe integer), so minimp3 sees frames.
size_t id3v2Size(const uint8_t* d, size_t len) {
  if (len < 10 || memcmp(d, "ID3", 3) != 0) return 0;
  if ((d[6] | d[7] | d[8] | d[9]) & 0x80) return 0;
  size_t n = 10 + (((size_t)d[6] << 21) | ((size_t)d[7] << 14) | ((size_t)d[8] << 7) | d[9]);
  if (d[5] & 0x10) n += 10;  // footer
  return n <= len ? n : len;
}

bool decodeMp3(const uint8_t* d, size_t len, uint32_t maxMs, const Clip::Allocator& a, Clip::Pcm* out,
               const char** error) {
  mp3dec_t* dec = (mp3dec_t*)a.alloc(sizeof(mp3dec_t));
  int16_t* frame = (int16_t*)a.alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
  int16_t* s = nullptr;
  size_t cap = 0, count = 0;
  uint32_t rate = 0;
  const char* why = nullptr;
  if (!dec || !frame) {
    why = "out of memory";
  } else {
    mp3dec_init(dec);
    size_t pos = id3v2Size(d, len);
    while (pos < len) {
      mp3dec_frame_info_t info;
      const int n = mp3dec_decode_frame(dec, d + pos, (int)(len - pos), frame, &info);
      if (info.frame_bytes <= 0) break;  // no further frame in the rest of the bytes
      pos += (size_t)info.frame_bytes;
      if (n <= 0) continue;  // skipped bytes (a tag or junk) or the decoder is filling its reservoir
      if (!s) {
        rate = (uint32_t)info.hz;
        if (rate < kMinRate || rate > kMaxRate) { why = "mp3: sample rate outside 8-48 kHz"; break; }
        cap = (size_t)((uint64_t)maxMs * rate / 1000u) + 1;
        s = (int16_t*)a.alloc(cap * sizeof(int16_t));
        if (!s) { why = "out of memory"; break; }
      } else if ((uint32_t)info.hz != rate) {
        why = "mp3: sample rate changes inside the clip";
        break;
      }
      if (count + (size_t)n > cap) { why = "clip longer than the limit"; break; }
      for (int i = 0; i < n; i++) {
        s[count + i] = info.channels == 2
                           ? (int16_t)(((int32_t)frame[2 * i] + (int32_t)frame[2 * i + 1]) / 2)
                           : frame[i];
      }
      count += (size_t)n;
    }
    if (!why && count == 0) why = "not a WAV PCM16 or MP3 clip";
  }
  if (dec) a.free(dec);
  if (frame) a.free(frame);
  if (why) {
    if (s) a.free(s);
    return fail(error, why);
  }
  out->samples = s;
  out->count = count;
  out->rate = rate;
  return true;
}

int b64(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

}  // namespace

namespace Clip {

bool decode(const uint8_t* data, size_t len, const char* hint, size_t maxBytes, uint32_t maxMs,
            const Allocator& a, Pcm* out, const char** error) {
  *out = Pcm{};
  if (!data || len == 0) return fail(error, "empty clip");
  if (len > maxBytes) return fail(error, "clip larger than the limit");
  const Kind k = kindOf(hint);
  if (k == Kind::Other) return fail(error, "unsupported format (WAV PCM16 or MP3 only)");
  if (isRiffWave(data, len)) {
    if (k == Kind::Mp3) return fail(error, "format says mp3 but the bytes are WAV");
    return decodeWav(data, len, maxMs, a, out, error);
  }
  if (k == Kind::Wav) return fail(error, "format says wav but the bytes are not RIFF/WAVE");
  return decodeMp3(data, len, maxMs, a, out, error);
}

bool base64Decode(const char* in, size_t inLen, uint8_t* out, size_t cap, size_t* outLen) {
  uint32_t acc = 0;
  int bitsHeld = 0;
  size_t n = 0;
  for (size_t i = 0; i < inLen; i++) {
    const char c = in[i];
    if (c == '=') break;
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\\') continue;
    const int v = b64(c);
    if (v < 0) return false;
    acc = (acc << 6) | (uint32_t)v;
    bitsHeld += 6;
    if (bitsHeld >= 8) {
      bitsHeld -= 8;
      if (n >= cap) return false;
      out[n++] = (uint8_t)(acc >> bitsHeld);
    }
  }
  *outLen = n;
  return true;
}

}  // namespace Clip
