/*
 * commandagi-seal: a device's sealer in portable C (docs/integrity.md § sealed streams). What a camera, a robot's
 * controller or any device firmware runs as it writes its own stream: it keeps the Merkle leaves of the lines and the
 * media bytes since its last seal, and emits a seal line that packages/domain/world/seals.js verifies byte for byte.
 *
 * The key is the device's: `sign` is a callback to whatever holds it (an SE050 or ATECC608B secure element, a phone
 * keystore, a software key in a test). It signs the exact bytes it is given and returns the raw 64-byte signature
 * (Ed25519, or ECDSA P-256 as r || s). No allocation, no I/O, no clock: the caller gives the time and writes the lines.
 */
#ifndef COMMANDAGI_SEAL_H
#define COMMANDAGI_SEAL_H
#include <stddef.h>
#include <stdint.h>

#ifndef SEAL_MAX_LINES
#define SEAL_MAX_LINES 1024 /* lines between two seals; seal more often than this */
#endif
#ifndef SEAL_MAX_CHUNKS
#define SEAL_MAX_CHUNKS 512 /* 64 KiB media chunks between two seals (32 MiB) */
#endif
#define SEAL_MEDIA_CHUNK 65536

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t fill; } seal_sha256;
void seal_sha256_init(seal_sha256 *s);
void seal_sha256_update(seal_sha256 *s, const uint8_t *data, size_t len);
void seal_sha256_final(seal_sha256 *s, uint8_t out[32]);

typedef int (*seal_sign_fn)(void *ctx, const uint8_t *msg, size_t len, uint8_t sig[64]);

typedef struct {
  const char *alg;       /* "ed25519" or "es256" */
  char key[72];          /* "sha256:<hex of the key's SubjectPublicKeyInfo>" */
  const char *announce;  /* the first seal's key announcement: a base64url SPKI, or a JSON array of base64 DER certificates */
  int announce_chain;    /* 1: announce is a certificate chain (JSON array text); 0: a bare SPKI */
  const char *clock;     /* "gnss", "rtc", "ntp", "host" or "none" */
  seal_sign_fn sign;
  void *sign_ctx;
  uint64_t counter;      /* the secure element's monotonic counter, when it has one: the next seal's */
  char prev[65];         /* the previous seal line's sha256 (hex); empty for none */
  int announced;
  uint8_t leaves[SEAL_MAX_LINES][32];
  size_t n;
  uint64_t first_seq;
  char media_file[64];
  uint64_t media_from, media_to;
  uint8_t media_leaves[SEAL_MAX_CHUNKS][32];
  size_t mn;
  seal_sha256 chunk;     /* the media chunk being filled: its leaf hash, so far */
  size_t chunk_fill;
} seal_ctx;

/* spki: the key's SubjectPublicKeyInfo (DER), to name it. */
void seal_init(seal_ctx *c, const char *alg, const uint8_t *spki, size_t spki_len, const char *announce, int announce_chain, const char *clock, seal_sign_fn sign, void *sign_ctx);
/* A line the device wrote (without its newline), with its seq. Returns 0, or -1 when the seal is full (seal first). */
int seal_line(seal_ctx *c, const char *line, size_t len, uint64_t seq);
/* Bytes appended to the stream's media file `file` at `offset` (which must be where the last append ended). */
int seal_media(seal_ctx *c, const char *file, uint64_t offset, const uint8_t *bytes, size_t len);
/*
 * The seal line for everything since the last seal: `t` its time (ISO), `seq` its own seq, `block_json` a recent
 * block as canonical JSON ({"chain":"solana:devnet","hash":"…","slot":123}) or NULL, `log_json` the contract log's
 * head as canonical JSON ({"head":"<64 hex>","seq":123}) or NULL, `status_json` what the device declares (a canonical
 * JSON object, keys sorted) or NULL. Writes the line (no newline) to `out`; returns its length, or -1 (the buffer is
 * too small, or the key would not sign).
 */
int seal_emit(seal_ctx *c, const char *t, uint64_t seq, const char *block_json, const char *log_json, const char *status_json, char *out, size_t cap);
#endif
