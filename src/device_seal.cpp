#include "device_seal.h"
#include <stdio.h>
#include <string.h>

namespace DeviceSeal {

namespace {
// The longest index line: two 20-digit numbers, a 64-hex digest, a 24-char time and the keys.
constexpr size_t kIndexLineMax = 256;

void hex(const uint8_t* b, size_t n, char* out) {
  static const char* x = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = x[b[i] >> 4];
    out[2 * i + 1] = x[b[i] & 15];
  }
  out[2 * n] = 0;
}
}  // namespace

size_t stampOverhead(const char* t) { return 4 + 2 + strlen(t); }

size_t stamp(const uint8_t* jpeg, size_t len, const char* t, uint8_t* out, size_t cap) {
  const size_t note = 2 + strlen(t), extra = 4 + note;
  if (len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 || note + 2 > 0xFFFF || cap < len + extra) return 0;
  out[0] = 0xFF;
  out[1] = 0xD8;
  out[2] = 0xFF;
  out[3] = 0xFE;  // COM; its length counts its own two bytes
  out[4] = (uint8_t)((note + 2) >> 8);
  out[5] = (uint8_t)((note + 2) & 0xFF);
  out[6] = 't';
  out[7] = '=';
  memcpy(out + 8, t, note - 2);
  memcpy(out + 6 + note, jpeg + 2, len - 2);
  return len + extra;
}

void Outbox::sent(size_t n) {
  if (n >= len) {
    len = 0;
  } else {
    memmove(buf, buf + n, len - n);
    len -= n;
  }
  buf[len] = 0;
}

bool Outbox::append(const char* line, size_t n) {
  if (len + n + 1 >= cap) return false;
  memcpy(buf + len, line, n);
  buf[len + n] = '\n';
  len += n + 1;
  buf[len] = 0;
  return true;
}

bool Stream::begin(const Config& cfg, seal_ctx* index, seal_ctx* media, char* indexOutbox, char* mediaOutbox, size_t outboxCap) {
  if (!index || !media || !indexOutbox || !mediaOutbox || outboxCap < 4096 || !cfg.sign || !cfg.spki || !cfg.alg ||
      cfg.firstSeq < 1 || cfg.firstMediaSeq < 1)
    return false;
  // Each stream's file announces the key in its own first seal.
  seal_init(index, cfg.alg, cfg.spki, cfg.spkiLen, cfg.announce, cfg.announceChain ? 1 : 0, "none", cfg.sign, cfg.signCtx);
  seal_init(media, cfg.alg, cfg.spki, cfg.spkiLen, cfg.announce, cfg.announceChain ? 1 : 0, "none", cfg.sign, cfg.signCtx);
  index_ = Part{};
  media_ = Part{};
  index_.ctx = index;
  media_.ctx = media;
  index_.out.buf = indexOutbox;
  media_.out.buf = mediaOutbox;
  index_.out.cap = media_.out.cap = outboxCap;
  indexOutbox[0] = mediaOutbox[0] = 0;
  index_.seq = cfg.firstSeq;
  media_.seq = cfg.firstMediaSeq;
  offset_ = 0;
  counter_ = cfg.counter;
  return true;
}

bool Stream::room(size_t stampedLen) const {
  const seal_ctx* m = media_.ctx;
  if (!index_.ctx || index_.ctx->n >= SEAL_MAX_LINES || index_.out.len + kIndexLineMax >= index_.out.cap) return false;
  // Leaves after this frame: the full chunks so far, the chunks it fills, and a part-filled tail.
  const uint64_t fill = (uint64_t)m->chunk_fill + stampedLen;
  const uint64_t leaves = m->mn + fill / SEAL_MEDIA_CHUNK + (fill % SEAL_MEDIA_CHUNK ? 1 : 0);
  return leaves <= SEAL_MAX_CHUNKS;
}

bool Stream::frameSent(const uint8_t* stamped, size_t len, const char* t, bool clockSet) {
  if (!room(len)) return false;
  seal_sha256 s;
  uint8_t digest[32];
  char digestHex[65], line[kIndexLineMax];
  seal_sha256_init(&s);
  seal_sha256_update(&s, stamped, len);
  seal_sha256_final(&s, digest);
  hex(digest, 32, digestHex);
  const int n = snprintf(line, sizeof line,
                         "{\"t\":\"%s\",\"seq\":%llu,\"src\":\"device\",\"kind\":\"event\",\"frame\":{\"offset\":%llu,\"length\":%llu,\"sha256\":\"%s\"}}",
                         t, (unsigned long long)index_.seq, (unsigned long long)offset_, (unsigned long long)len, digestHex);
  if (n <= 0 || (size_t)n >= sizeof line) return false;
  // room() said all three fit, so none of these can fail.
  if (seal_media(media_.ctx, kMediaFile, offset_, stamped, len) != 0) return false;
  seal_line(index_.ctx, line, (size_t)n, index_.seq);
  index_.out.append(line, (size_t)n);
  offset_ += len;
  index_.seq++;
  if (!clockSet) index_.clockUnset = media_.clockUnset = true;
  return true;
}

bool Stream::Part::pending() const {
  return ctx && (ctx->n > 0 || (ctx->media_file[0] && ctx->media_to > ctx->media_from));
}

bool Stream::Part::seal(const char* t, bool clockSet, const char* block, const char* status, uint64_t& counter) {
  if (out.len + 2 >= out.cap) return false;
  ctx->clock = clockSet && !clockUnset ? "ntp" : "none";
  ctx->counter = counter;
  char* at = out.buf + out.len;
  const int n = seal_emit(ctx, t, seq, block, status, at, out.cap - out.len - 1);
  if (n < 0) {
    out.buf[out.len] = 0;
    return false;
  }
  at[n] = '\n';
  out.len += (size_t)n + 1;
  out.buf[out.len] = 0;
  counter = ctx->counter;
  seq++;
  clockUnset = false;
  return true;
}

bool Stream::pending() const { return index_.pending() || media_.pending(); }

bool Stream::seal(const char* t, bool clockSet, const char* block, const char* status) {
  bool ok = true;
  if (media_.pending()) ok = media_.seal(t, clockSet, block, status, counter_) && ok;
  if (index_.pending()) ok = index_.seal(t, clockSet, block, status, counter_) && ok;
  return ok;
}

}  // namespace DeviceSeal
