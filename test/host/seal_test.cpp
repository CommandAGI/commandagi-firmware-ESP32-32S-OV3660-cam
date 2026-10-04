// Host test of the camera's sealed streams (src/device_seal.cpp over src/seal/seal.c): a camera sends
// frames for three seconds and seals once a second. The recorder keeps none of a fourth second (a frame
// was lost on the way) and says where its files stand (`seal_resume`); the camera resumes from there and
// sends a fifth. It prints `cam-at`'s records.jsonl on stdout, and
// writes `cam`'s video.mjpeg to argv[1] and its records.jsonl (its seals) to argv[2]. The CommandAGI
// repository's tests/workbench/seal-c.test.mjs builds and runs it, and its JavaScript verifier
// (packages/domain/world/seals.js verifySeals) must accept the files as they are. TweetNaCl stands in for libsodium's Ed25519 (the same signature scheme):
//
//   cc -O2 -c -I<seal-c>/test <seal-c>/test/tweetnacl.c -o /tmp/tweetnacl.o
//   cc -O2 -c src/seal/seal.c -o /tmp/seal.o
//   g++ -std=c++11 -O2 -Wall -Werror -Isrc -I<seal-c>/test test/host/seal_test.cpp src/device_seal.cpp
//       /tmp/seal.o /tmp/tweetnacl.o -o /tmp/seal_test && /tmp/seal_test /tmp/video.mjpeg /tmp/cam.jsonl
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "chip_lock_status.h"
#include "device_seal.h"
extern "C" {
#include "tweetnacl.h"
void randombytes(unsigned char* x, unsigned long long n) {
  FILE* f = fopen("/dev/urandom", "rb");
  if (!f || fread(x, 1, (size_t)n, f) != n) exit(2);
  fclose(f);
}
}

namespace {
unsigned char g_sk[64];
int sign(void*, const uint8_t* msg, size_t len, uint8_t sig[64]) {
  std::vector<unsigned char> sm(len + 64);
  unsigned long long smlen = 0;
  if (crypto_sign(sm.data(), &smlen, msg, len, g_sk)) return -1;
  memcpy(sig, sm.data(), 64);
  return 0;
}
void b64u(const uint8_t* b, size_t n, char* out) {
  static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
    out[o++] = a[v >> 18 & 63];
    out[o++] = a[v >> 12 & 63];
    if (i + 1 < n) out[o++] = a[v >> 6 & 63];
    if (i + 2 < n) out[o++] = a[v & 63];
  }
  out[o] = 0;
}
// A JPEG-shaped frame: SOI, an empty SOS header, entropy-coded bytes that never form a marker, EOI.
std::vector<uint8_t> jpeg(size_t len, int k) {
  std::vector<uint8_t> f(len);
  for (size_t i = 0; i < len; i++) f[i] = (uint8_t)((i * 7 + k) % 200);
  const uint8_t head[] = {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x02};
  memcpy(f.data(), head, sizeof head);
  f[len - 2] = 0xFF;
  f[len - 1] = 0xD9;
  return f;
}
void fail(const char* why) {
  fprintf(stderr, "FAIL %s\n", why);
  exit(1);
}
// The sha256 (hex) of the last line in `buf`: a seal, whose hash the next seal's prev names.
std::string lastLineHash(const char* buf, size_t len) {
  std::string text(buf, len);
  while (!text.empty() && text.back() == '\n') text.pop_back();
  const size_t at = text.rfind('\n');
  const std::string line = at == std::string::npos ? text : text.substr(at + 1);
  seal_sha256 h;
  uint8_t d[32];
  seal_sha256_init(&h);
  seal_sha256_update(&h, (const uint8_t*)line.data(), line.size());
  seal_sha256_final(&h, d);
  char out[65];
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
  return out;
}
// The seq of the last line in `buf`.
uint64_t lastLineSeq(const char* buf, size_t len) {
  std::string text(buf, len);
  while (!text.empty() && text.back() == '\n') text.pop_back();
  const size_t at = text.rfind("\"seq\":");
  return at == std::string::npos ? 0 : strtoull(text.c_str() + at + 6, nullptr, 10);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  unsigned char pk[32];
  crypto_sign_keypair(pk, g_sk);
  uint8_t spki[44] = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};
  memcpy(spki + 12, pk, 32);
  char announce[64];
  b64u(spki, sizeof spki, announce);

  static seal_ctx index, mediaCtx;
  static char indexOut[65536], mediaOut[65536];
  DeviceSeal::Stream s;
  // A camera that rebooted: its seqs and its counter go on from where its last reservations ended.
  const DeviceSeal::Config cfg = {"ed25519", spki, sizeof spki, announce, false, sign, nullptr, 4097, 1025, 2048};
  if (!s.begin(cfg, &index, &mediaCtx, indexOut, mediaOut, sizeof indexOut)) fail("begin");

  // Stamping refuses what is not a JPEG and a buffer that is too small.
  uint8_t small[8];
  const uint8_t notJpeg[6] = {0, 1, 2, 3, 4, 5};
  if (DeviceSeal::stamp(notJpeg, sizeof notJpeg, "2026-10-03T12:00:00.000Z", small, sizeof small)) fail("stamped a non-JPEG");
  const std::vector<uint8_t> tiny = jpeg(8, 0);
  if (DeviceSeal::stamp(tiny.data(), tiny.size(), "2026-10-03T12:00:00.000Z", small, sizeof small)) fail("stamped into a short buffer");
  if (!s.seal("2026-10-03T12:00:00.000Z", true, nullptr, nullptr) || s.counter() != 2048 || s.indexOutbox().len ||
      s.mediaOutbox().len)
    fail("sealed nothing");

  FILE* media = fopen(argv[1], "wb");
  FILE* cam = fopen(argv[2], "wb");
  if (!media || !cam) return 2;
  const char* block = "{\"chain\":\"solana:devnet\",\"hash\":\"9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb9PusVFin\",\"slot\":4242}";
  // A dev unit's status (nothing locked), then a locked unit's: each verifies as the seal's own field.
  using ChipLockStatus::Download;
  using ChipLockStatus::Flash;
  char devStatus[160], lockedStatus[160], shortBuf[40];
  if (!ChipLockStatus::json({false, Flash::Plain, false, false, Download::Open}, "1.3.0", devStatus, sizeof devStatus) ||
      !ChipLockStatus::json({true, Flash::Release, true, true, Download::Secure}, "1.3.0", lockedStatus, sizeof lockedStatus) ||
      ChipLockStatus::json({true, Flash::Release, true, true, Download::Secure}, "1.3.0", shortBuf, sizeof shortBuf))
    fail("status");
  std::vector<uint8_t> stamped;
  // What the recorder kept last of each stream: its last seal's hash and seq, and the media file's length.
  std::string indexPrev, camPrev;
  uint64_t indexSeq = 0, camSeq = 0, mediaBytes = 0;
  for (int sec = 0; sec < 5; sec++) {
    const bool refused = sec == 3;
    for (int k = 0; k < 4; k++) {
      char t[32];
      snprintf(t, sizeof t, "2026-10-03T12:00:%02d.%03dZ", sec, 100 + 200 * k);
      // Sizes that cross the 64 KiB chunk boundaries at different places.
      const std::vector<uint8_t> f = jpeg(9000 + 23000 * (size_t)k + 1234 * (size_t)sec, sec * 4 + k);
      stamped.resize(f.size() + DeviceSeal::stampOverhead(t));
      const size_t n = DeviceSeal::stamp(f.data(), f.size(), t, stamped.data(), stamped.size());
      if (n != stamped.size()) fail("stamp length");
      if (!s.room(n)) fail("no room");
      // The second frame of the first second is lost before it is sent: it is not indexed or sealed.
      if (sec == 0 && k == 1) continue;
      if (!refused) {
        fwrite(stamped.data(), 1, n, media);
        mediaBytes += n;
      }
      // The clock was not set yet during the first second.
      if (!s.frameSent(stamped.data(), n, t, sec > 0)) fail("frameSent");
    }
    char t[32];
    snprintf(t, sizeof t, "2026-10-03T12:00:%02d.950Z", sec);
    if (!s.seal(t, true, sec ? block : nullptr, sec < 3 ? devStatus : lockedStatus)) fail("seal");
    if (refused) {
      // The recorder refused cam's group: it keeps nothing more of either stream until the camera resumes.
      const DeviceSeal::FileState ix{indexSeq, indexPrev.c_str(), 0}, mx{camSeq, camPrev.c_str(), mediaBytes};
      const uint64_t seqBefore = s.nextSeq(), counterBefore = s.counter();
      s.resume(&ix, &mx, 2053);
      if (s.indexOutbox().len || s.mediaOutbox().len || s.pending()) fail("resume kept what the recorder dropped");
      if (s.mediaOffset() != mediaBytes) fail("resume: the media offset is not the file's length");
      if (s.nextSeq() != seqBefore || s.counter() != counterBefore) fail("resume took a seq or the counter back");
      continue;
    }
    // The index goes out in two parts in the last second, as a socket might take it.
    DeviceSeal::Outbox& ix = s.indexOutbox();
    if (sec == 2) {
      const size_t first = (size_t)(strchr(ix.buf, '\n') - ix.buf) + 1;
      fwrite(ix.buf, 1, first, stdout);
      ix.sent(first);
    }
    fwrite(ix.buf, 1, ix.len, stdout);
    indexPrev = lastLineHash(ix.buf, ix.len);
    indexSeq = lastLineSeq(ix.buf, ix.len);
    ix.sent(ix.len);
    // The frames' seals wait one second while the socket is down.
    DeviceSeal::Outbox& mx = s.mediaOutbox();
    if (sec != 1) {
      fwrite(mx.buf, 1, mx.len, cam);
      camPrev = lastLineHash(mx.buf, mx.len);
      camSeq = lastLineSeq(mx.buf, mx.len);
      mx.sent(mx.len);
    }
  }
  fclose(media);
  fclose(cam);
  if (s.indexOutbox().len || s.mediaOutbox().len) fail("an outbox kept lines");
  // Five seconds of seqs, the refused one's skipped, never reused; two seals a second.
  if (s.nextSeq() != 4097 + 19 + 5 || s.nextMediaSeq() != 1025 + 5 || s.counter() != 2048 + 10) fail("seq or counter");
  return 0;
}
