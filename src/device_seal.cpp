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

bool Stream::begin(const Config& cfg, seal_ctx* ctx, char* outbox, size_t outboxCap) {
  if (!ctx || !outbox || outboxCap < 1024 || !cfg.sign || !cfg.spki || !cfg.alg || cfg.firstSeq < 1) return false;
  seal_init(ctx, cfg.alg, cfg.spki, cfg.spkiLen, cfg.announce, cfg.announceChain ? 1 : 0, "none", cfg.sign, cfg.signCtx);
  ctx->counter = cfg.counter;
  ctx_ = ctx;
  outbox_ = outbox;
  outboxCap_ = outboxCap;
  outboxLen_ = 0;
  outbox_[0] = 0;
  seq_ = cfg.firstSeq;
  offset_ = 0;
  clockUnset_ = false;
  return true;
}

bool Stream::room(size_t stampedLen) const {
  if (!ctx_ || ctx_->n >= SEAL_MAX_LINES || outboxLen_ + kIndexLineMax >= outboxCap_) return false;
  // Leaves after this frame: the full chunks so far, the chunks it fills, and a part-filled tail.
  const uint64_t fill = (uint64_t)ctx_->chunk_fill + stampedLen;
  const uint64_t leaves = ctx_->mn + fill / SEAL_MEDIA_CHUNK + (fill % SEAL_MEDIA_CHUNK ? 1 : 0);
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
                         t, (unsigned long long)seq_, (unsigned long long)offset_, (unsigned long long)len, digestHex);
  if (n <= 0 || (size_t)n >= sizeof line) return false;
  // room() said both fit, so neither of these can fail; the media goes first because it is the larger.
  if (seal_media(ctx_, kMediaFile, offset_, stamped, len) != 0) return false;
  seal_line(ctx_, line, (size_t)n, seq_);
  append(line, (size_t)n);
  offset_ += len;
  seq_++;
  if (!clockSet) clockUnset_ = true;
  return true;
}

bool Stream::pending() const { return ctx_ && (ctx_->n > 0 || (ctx_->media_file[0] && ctx_->media_to > ctx_->media_from)); }

bool Stream::seal(const char* t, bool clockSet, const char* block, const char* status) {
  if (!pending() || outboxLen_ + 2 >= outboxCap_) return false;
  ctx_->clock = clockSet && !clockUnset_ ? "ntp" : "none";
  char* at = outbox_ + outboxLen_;
  const int n = seal_emit(ctx_, t, seq_, block, status, at, outboxCap_ - outboxLen_ - 1);
  if (n < 0) {
    outbox_[outboxLen_] = 0;
    return false;
  }
  at[n] = '\n';
  outboxLen_ += (size_t)n + 1;
  outbox_[outboxLen_] = 0;
  seq_++;
  clockUnset_ = false;
  return true;
}

bool Stream::append(const char* line, size_t len) {
  if (outboxLen_ + len + 1 >= outboxCap_) return false;
  memcpy(outbox_ + outboxLen_, line, len);
  outbox_[outboxLen_ + len] = '\n';
  outboxLen_ += len + 1;
  outbox_[outboxLen_] = 0;
  return true;
}

void Stream::sent(size_t n) {
  if (n >= outboxLen_) {
    outboxLen_ = 0;
  } else {
    memmove(outbox_, outbox_ + n, outboxLen_ - n);
    outboxLen_ -= n;
  }
  outbox_[outboxLen_] = 0;
}

}  // namespace DeviceSeal
