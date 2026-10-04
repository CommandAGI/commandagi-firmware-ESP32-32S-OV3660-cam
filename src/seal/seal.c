/* commandagi-seal: see seal.h. The seal line is canonical JSON (keys sorted), so what is signed is the line without "sig". */
#include "seal.h"
#include <string.h>
#include <stdio.h>

static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
  0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
  0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void block(seal_sha256 *s, const uint8_t *p) {
  uint32_t w[64], a, b, c, d, e, f, g, h;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) w[i] = (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
void seal_sha256_init(seal_sha256 *s) {
  static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  memcpy(s->h, iv, sizeof iv); s->len = 0; s->fill = 0;
}
void seal_sha256_update(seal_sha256 *s, const uint8_t *d, size_t n) {
  s->len += n;
  while (n) {
    size_t k = 64 - s->fill < n ? 64 - s->fill : n;
    memcpy(s->buf + s->fill, d, k); s->fill += k; d += k; n -= k;
    if (s->fill == 64) { block(s, s->buf); s->fill = 0; }
  }
}
void seal_sha256_final(seal_sha256 *s, uint8_t out[32]) {
  uint64_t bits = s->len * 8; uint8_t pad = 0x80, zero = 0, lenb[8];
  seal_sha256_update(s, &pad, 1);
  while (s->fill != 56) seal_sha256_update(s, &zero, 1);
  for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
  seal_sha256_update(s, lenb, 8);
  for (int i = 0; i < 8; i++) { out[4 * i] = s->h[i] >> 24; out[4 * i + 1] = s->h[i] >> 16; out[4 * i + 2] = s->h[i] >> 8; out[4 * i + 3] = s->h[i]; }
}
static void sha(const uint8_t *d, size_t n, uint8_t out[32]) { seal_sha256 s; seal_sha256_init(&s); seal_sha256_update(&s, d, n); seal_sha256_final(&s, out); }
static void hexof(const uint8_t *b, size_t n, char *out) { static const char *x = "0123456789abcdef"; for (size_t i = 0; i < n; i++) { out[2 * i] = x[b[i] >> 4]; out[2 * i + 1] = x[b[i] & 15]; } out[2 * n] = 0; }
/* RFC 9162: a leaf is sha256(0x00 ‖ data), a node sha256(0x01 ‖ left ‖ right). */
static void leaf_begin(seal_sha256 *s) { uint8_t z = 0; seal_sha256_init(s); seal_sha256_update(s, &z, 1); }
static void node(const uint8_t l[32], const uint8_t r[32], uint8_t out[32]) { uint8_t b[65]; b[0] = 1; memcpy(b + 1, l, 32); memcpy(b + 33, r, 32); sha(b, 65, out); }
static void root(uint8_t leaves[][32], size_t n, uint8_t out[32]) {
  if (n == 0) { sha((const uint8_t *)"", 0, out); return; }
  if (n == 1) { memcpy(out, leaves[0], 32); return; }
  size_t k = 1; while (k * 2 < n) k *= 2;
  uint8_t l[32], r[32]; root(leaves, k, l); root(leaves + k, n - k, r); node(l, r, out);
}
void seal_init(seal_ctx *c, const char *alg, const uint8_t *spki, size_t spki_len, const char *announce, int announce_chain, const char *clock, seal_sign_fn sign, void *sign_ctx) {
  memset(c, 0, sizeof *c);
  uint8_t h[32]; sha(spki, spki_len, h);
  memcpy(c->key, "sha256:", 7); hexof(h, 32, c->key + 7);
  c->alg = alg; c->announce = announce; c->announce_chain = announce_chain; c->clock = clock; c->sign = sign; c->sign_ctx = sign_ctx;
}
int seal_line(seal_ctx *c, const char *line, size_t len, uint64_t seq) {
  if (c->n >= SEAL_MAX_LINES) return -1;
  if (c->n == 0) c->first_seq = seq;
  seal_sha256 s; leaf_begin(&s); seal_sha256_update(&s, (const uint8_t *)line, len); seal_sha256_final(&s, c->leaves[c->n++]);
  return 0;
}
int seal_media(seal_ctx *c, const char *file, uint64_t offset, const uint8_t *b, size_t n) {
  if (!c->media_file[0]) { snprintf(c->media_file, sizeof c->media_file, "%s", file); c->media_from = c->media_to = offset; leaf_begin(&c->chunk); }
  if (strcmp(c->media_file, file) || offset != c->media_to) return -1;
  while (n) {
    size_t k = SEAL_MEDIA_CHUNK - c->chunk_fill < n ? SEAL_MEDIA_CHUNK - c->chunk_fill : n;
    seal_sha256_update(&c->chunk, b, k); c->chunk_fill += k; b += k; n -= k; c->media_to += k;
    if (c->chunk_fill == SEAL_MEDIA_CHUNK) {
      if (c->mn >= SEAL_MAX_CHUNKS) return -1;
      seal_sha256_final(&c->chunk, c->media_leaves[c->mn++]); leaf_begin(&c->chunk); c->chunk_fill = 0;
    }
  }
  return 0;
}
/* The canonical record (keys sorted); with sig when `sig` is set. */
static int compose(seal_ctx *c, char *out, size_t cap, const char *t, uint64_t seq, const char *block_json, const char *log_json, const char *status_json, const char *rootx, const char *mediax, const char *sig) {
  char prev[80]; if (c->prev[0]) snprintf(prev, sizeof prev, "\"%s\"", c->prev); else snprintf(prev, sizeof prev, "null");
  uint64_t from = c->n ? c->first_seq : seq, to = c->n ? c->first_seq + c->n - 1 : seq - 1;
  char ann[16] = "";
  int n = snprintf(out, cap, "{\"kind\":\"seal\",\"seal\":{\"alg\":\"%s\"%s%s%s", c->alg, block_json ? ",\"block\":" : "", block_json ? block_json : "",
                   !c->announced && c->announce && c->announce_chain ? ",\"chain\":" : "");
  if (n < 0 || (size_t)n >= cap) return -1;
  (void)ann;
  int m = snprintf(out + n, cap - n, "%s,\"clock\":\"%s\",\"counter\":%llu,\"from\":%llu,\"key\":\"%s\"%s%s%s,\"prev\":%s,\"root\":\"%s\"%s%s%s%s%s%s%s,\"to\":%llu},\"seq\":%llu,\"src\":\"device\",\"t\":\"%s\"}",
                   !c->announced && c->announce && c->announce_chain ? c->announce : "", c->clock, (unsigned long long)c->counter, (unsigned long long)from, c->key,
                   log_json ? ",\"log\":" : "", log_json ? log_json : "", mediax, prev, rootx,
                   sig ? ",\"sig\":\"" : "", sig ? sig : "", sig ? "\"" : "",
                   !c->announced && c->announce && !c->announce_chain ? ",\"spki\":\"" : "", !c->announced && c->announce && !c->announce_chain ? c->announce : "", !c->announced && c->announce && !c->announce_chain ? "\"" : "",
                   status_json ? "" : "", (unsigned long long)to, (unsigned long long)seq, t);
  if (m < 0 || (size_t)(n + m) >= cap) return -1;
  /* status sorts between spki and to: splice it in before the seal's own ,"to":, the last one (media has a "to" too). */
  if (status_json) {
    char *at = NULL;
    for (char *p = strstr(out + n, ",\"to\":"); p; p = strstr(p + 1, ",\"to\":")) at = p;
    size_t add = strlen(",\"status\":") + strlen(status_json), len = (size_t)(n + m);
    if (!at || len + add >= cap) return -1;
    memmove(at + add, at, len - (size_t)(at - out) + 1);
    memcpy(at, ",\"status\":", 10); memcpy(at + 10, status_json, strlen(status_json));
    m += (int)add;
  }
  return n + m;
}
static void b64u(const uint8_t *b, size_t n, char *out) {
  static const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
    out[o++] = a[v >> 18 & 63]; out[o++] = a[v >> 12 & 63];
    if (i + 1 < n) out[o++] = a[v >> 6 & 63];
    if (i + 2 < n) out[o++] = a[v & 63];
  }
  out[o] = 0;
}
int seal_emit(seal_ctx *c, const char *t, uint64_t seq, const char *block_json, const char *log_json, const char *status_json, char *out, size_t cap) {
  uint8_t r[32]; char rootx[65], mediax[256] = "";
  root(c->leaves, c->n, r); hexof(r, 32, rootx);
  if (c->media_file[0] && c->media_to > c->media_from) {
    uint8_t leaves_tail[32]; size_t mn = c->mn;
    if (c->chunk_fill) { seal_sha256 copy = c->chunk; seal_sha256_final(&copy, leaves_tail); memcpy(c->media_leaves[mn++], leaves_tail, 32); }
    uint8_t mr[32]; char mrx[65]; root(c->media_leaves, mn, mr); hexof(mr, 32, mrx);
    snprintf(mediax, sizeof mediax, ",\"media\":{\"file\":\"%s\",\"from\":%llu,\"root\":\"%s\",\"to\":%llu}", c->media_file, (unsigned long long)c->media_from, mrx, (unsigned long long)c->media_to);
  }
  static const char domain[] = "commandagi.seal\n";
  size_t dl = sizeof domain - 1;
  if (cap < dl + 1) return -1;
  int n = compose(c, out + dl, cap - dl, t, seq, block_json, log_json, status_json, rootx, mediax, NULL);
  if (n < 0) return -1;
  memcpy(out, domain, dl);
  uint8_t sig[64]; char sigx[90];
  if (c->sign(c->sign_ctx, (const uint8_t *)out, dl + (size_t)n, sig)) return -1;
  b64u(sig, 64, sigx);
  n = compose(c, out, cap, t, seq, block_json, log_json, status_json, rootx, mediax, sigx);
  if (n < 0) return -1;
  uint8_t h[32]; sha((const uint8_t *)out, (size_t)n, h); hexof(h, 32, c->prev);
  c->counter++; c->announced = 1; c->n = 0;
  if (c->media_file[0]) { c->media_from = c->media_to; c->mn = 0; c->chunk_fill = 0; leaf_begin(&c->chunk); }
  return n;
}
