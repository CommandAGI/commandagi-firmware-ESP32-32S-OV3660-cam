// Host test of the speaker clip decoder (src/clip.cpp): the clip comes from the network, so the
// checks on size, length, format and WAV structure must hold for bad input. Run from the repo root:
//
//   g++ -std=c++17 -O1 -Wall -Isrc test/host/clip_test.cpp src/clip.cpp -o /tmp/clip_test && /tmp/clip_test [file.mp3 ...]
//
// Each MP3 path given on the command line must decode (minimp3's own vectors/*.bit work).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "clip.h"

namespace {
int g_failed = 0;
const Clip::Allocator kHeap = {malloc, free};
constexpr size_t kMaxBytes = 1024 * 1024;
constexpr uint32_t kMaxMs = 30000;

void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xFF); }
void tag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

struct WavSpec {
  uint16_t format = 1, channels = 1, bits = 16;
  uint32_t rate = 16000;
  size_t frames = 1600;
  bool fmt = true;
  bool listBeforeFmt = false;  // an odd-sized chunk first: tests the pad byte
  int32_t dataSizeDelta = 0;   // a data size that lies about the bytes that follow
};

std::vector<uint8_t> wav(const WavSpec& s) {
  std::vector<uint8_t> v;
  tag(v, "RIFF");
  put32(v, 0);  // RIFF size: the decoder does not trust it
  tag(v, "WAVE");
  if (s.listBeforeFmt) {
    tag(v, "LIST");
    put32(v, 3);
    v.push_back('a'); v.push_back('b'); v.push_back('c'); v.push_back(0);  // + pad byte
  }
  if (s.fmt) {
    tag(v, "fmt ");
    put32(v, 16);
    put16(v, s.format);
    put16(v, s.channels);
    put32(v, s.rate);
    put32(v, s.rate * s.channels * s.bits / 8);
    put16(v, s.channels * s.bits / 8);
    put16(v, s.bits);
  }
  const size_t bytes = s.frames * s.channels * s.bits / 8;
  tag(v, "data");
  put32(v, (uint32_t)((int64_t)bytes + s.dataSizeDelta));
  for (size_t i = 0; i < s.frames; i++)
    for (int c = 0; c < s.channels; c++) {
      if (s.bits == 16) put16(v, (uint16_t)(int16_t)(c == 0 ? 1000 : 3000));
      else v.push_back(0x80);
    }
  return v;
}

void expectOk(const char* name, const std::vector<uint8_t>& d, const char* hint, size_t count, uint32_t rate,
              int16_t first) {
  Clip::Pcm p;
  const char* err = nullptr;
  bool ok = Clip::decode(d.data(), d.size(), hint, kMaxBytes, kMaxMs, kHeap, &p, &err);
  if (!ok || p.count != count || p.rate != rate || (count && p.samples[0] != first)) {
    std::printf("FAIL %s: ok=%d err=%s count=%zu rate=%u\n", name, ok, err ? err : "-", p.count, p.rate);
    g_failed++;
  } else {
    std::printf("ok   %s\n", name);
  }
  if (ok) free(p.samples);
}

void expectRefused(const char* name, const std::vector<uint8_t>& d, const char* hint, const char* why,
                   size_t maxBytes = kMaxBytes) {
  Clip::Pcm p;
  const char* err = nullptr;
  bool ok = Clip::decode(d.data(), d.size(), hint, maxBytes, kMaxMs, kHeap, &p, &err);
  if (ok || !err || std::strstr(err, why) == nullptr) {
    std::printf("FAIL %s: ok=%d err=%s (want '%s')\n", name, ok, err ? err : "-", why);
    g_failed++;
    if (ok) free(p.samples);
  } else {
    std::printf("ok   %s -> %s\n", name, err);
  }
}

std::vector<uint8_t> readFile(const char* path) {
  std::vector<uint8_t> v;
  FILE* f = std::fopen(path, "rb");
  if (!f) return v;
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
  std::fclose(f);
  return v;
}
}  // namespace

int main(int argc, char** argv) {
  expectOk("wav mono 16 kHz", wav({}), nullptr, 1600, 16000, 1000);
  { WavSpec s; s.channels = 2; s.rate = 8000; expectOk("wav stereo -> mono average", wav(s), "audio/wav", 1600, 8000, 2000); }
  { WavSpec s; s.listBeforeFmt = true; expectOk("wav odd chunk before fmt (pad byte)", wav(s), "wav", 1600, 16000, 1000); }
  { WavSpec s; s.frames = 16000 * 30; expectOk("wav exactly 30 s at 16 kHz (960 kB)", wav(s), nullptr, 16000 * 30, 16000, 1000); }
  { WavSpec s; s.rate = 48000; s.frames = 48000 * 20; expectRefused("wav 20 s at 48 kHz (1.9 MB)", wav(s), nullptr, "larger"); }

  { WavSpec s; s.bits = 8; expectRefused("wav 8-bit", wav(s), nullptr, "not 16-bit"); }
  { WavSpec s; s.format = 3; expectRefused("wav float", wav(s), nullptr, "not PCM"); }
  { WavSpec s; s.channels = 3; expectRefused("wav 3 channels", wav(s), nullptr, "mono or stereo"); }
  { WavSpec s; s.rate = 96000; expectRefused("wav 96 kHz", wav(s), nullptr, "8-48 kHz"); }
  { WavSpec s; s.rate = 4000; expectRefused("wav 4 kHz", wav(s), nullptr, "8-48 kHz"); }
  { WavSpec s; s.rate = 8000; s.frames = 8000 * 31; expectRefused("wav 31 s", wav(s), nullptr, "longer"); }
  { WavSpec s; s.fmt = false; expectRefused("wav without fmt", wav(s), nullptr, "no fmt"); }
  { WavSpec s; s.dataSizeDelta = 2; expectRefused("wav data size past the end", wav(s), nullptr, "past the end"); }
  { WavSpec s; s.dataSizeDelta = -0x7fffffff; expectRefused("wav huge data size", wav(s), nullptr, "past the end"); }
  { WavSpec s; s.frames = 0; expectRefused("wav no samples", wav(s), nullptr, "no samples"); }
  expectRefused("wav over the byte limit", wav({}), nullptr, "larger", 100);
  expectRefused("wav bytes, mp3 format", wav({}), "mp3", "bytes are WAV");
  expectRefused("unsupported format hint", wav({}), "audio/ogg", "unsupported");
  {
    std::vector<uint8_t> junk(5000);
    uint32_t x = 12345;
    for (auto& b : junk) { x = x * 1103515245u + 12345u; b = (uint8_t)(x >> 16); }
    expectRefused("random bytes", junk, nullptr, "not a WAV PCM16 or MP3");
    expectRefused("random bytes, wav format", junk, "wav", "not RIFF");
  }
  expectRefused("empty", std::vector<uint8_t>(), nullptr, "empty");

  {
    const std::string text = "UklGRiQAAABXQVZF";  // "RIFF$\0\0\0WAVE"
    uint8_t out[16];
    size_t n = 0;
    bool ok = Clip::base64Decode(text.data(), text.size(), out, sizeof out, &n);
    bool good = ok && n == 12 && std::memcmp(out, "RIFF$\0\0\0WAVE", 12) == 0;
    std::string esc = "QUJD\\/w==";  // "ABC" then "\/" (JSON escape of '/') then "w=="
    uint8_t o2[8];
    size_t n2 = 0;
    good = good && Clip::base64Decode(esc.data(), esc.size(), o2, sizeof o2, &n2) && n2 == 4 && o2[3] == 0xFF;
    good = good && !Clip::base64Decode("QUJD", 4, out, 2, &n);   // over the cap
    good = good && !Clip::base64Decode("QU*D", 4, out, 16, &n);  // bad character
    std::printf("%s base64\n", good ? "ok  " : "FAIL");
    if (!good) g_failed++;
  }

  for (int i = 1; i < argc; i++) {
    std::vector<uint8_t> d = readFile(argv[i]);
    Clip::Pcm p;
    const char* err = nullptr;
    bool ok = Clip::decode(d.data(), d.size(), "mp3", 16 * kMaxBytes, 600000, kHeap, &p, &err);
    std::printf("%s mp3 %s: %s, %zu samples at %u Hz (%u ms)\n", ok ? "ok  " : "FAIL", argv[i], ok ? "decoded" : err,
                p.count, p.rate, p.durationMs());
    if (!ok) g_failed++;
    if (ok) {
      const uint32_t half = p.durationMs() / 2;
      free(p.samples);
      Clip::Pcm q;
      bool shortOk = Clip::decode(d.data(), d.size(), "mp3", 16 * kMaxBytes, half, kHeap, &q, &err);
      std::printf("%s mp3 %s with a %u ms limit: %s\n", !shortOk ? "ok  " : "FAIL", argv[i], half, shortOk ? "accepted" : err);
      if (shortOk) { g_failed++; free(q.samples); }
    }
  }

  std::printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
