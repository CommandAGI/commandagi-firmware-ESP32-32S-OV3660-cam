#pragma once
// The camera's sealed stream, without Arduino (so test/host/seal_test.cpp builds it on a PC).
//
// The device seals its own `cam` frames and its `cam-at` index (docs/integrity.md in the CommandAGI
// repository § sealed streams). Per frame it sent:
//   - the frame is stamped first: a JPEG COM segment `t=<ISO>` right after SOI (mjpeg.js stampFrame);
//   - those exact bytes grow `video.mjpeg` (the `cam` stream's media) and the sealer's media tree;
//   - one `cam-at` line names them: {"t","seq","src":"device","kind":"event","frame":{offset,length,sha256}}.
// About once a second a seal line covers the index lines and the media bytes since the last seal. Index
// lines and seal lines go out in order through the outbox; the recorder keeps them byte for byte.
#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "seal/seal.h"
}

namespace DeviceSeal {

// The media file a `video` stream's frames grow (the recorder names it so: thread-run.ts).
constexpr const char* kMediaFile = "video.mjpeg";

struct Config {
  const char* alg;        // "ed25519"
  const uint8_t* spki;    // the key's SubjectPublicKeyInfo (DER)
  size_t spkiLen;
  const char* announce;   // base64url SPKI, or a JSON array of base64 DER certificates (leaf first)
  bool announceChain;
  seal_sign_fn sign;
  void* signCtx;
  uint64_t firstSeq;      // the first line's seq (see Seal::begin: seqs never go back across reboots)
  uint64_t counter;       // the next seal's counter
};

// The bytes a stamp adds to a frame: FF FE, a 2-byte length, "t=", the time.
size_t stampOverhead(const char* t);
// SOI, the COM segment `t=<t>`, then the rest of `jpeg`. Returns the stamped length, or 0 when `jpeg`
// does not start with SOI or `out` is too small.
size_t stamp(const uint8_t* jpeg, size_t len, const char* t, uint8_t* out, size_t cap);

class Stream {
 public:
  // `ctx` is the sealer's state (about 49 kB at the default SEAL_MAX_*); the caller owns its memory.
  // `outbox` holds lines not yet sent. Returns false when the arguments are unusable.
  bool begin(const Config& cfg, seal_ctx* ctx, char* outbox, size_t outboxCap);

  // Whether a frame of `stampedLen` bytes fits in the current seal and its index line in the outbox.
  // When it does not, seal first (and send the outbox); a frame is never sent that cannot be indexed.
  bool room(size_t stampedLen) const;
  // A stamped frame that was sent: grow the media tree and write its index line. `t` is its stamp.
  // `clockSet` false makes the next seal say clock "none". Returns false (and indexes nothing) when
  // room() would have said no.
  bool frameSent(const uint8_t* stamped, size_t len, const char* t, bool clockSet);

  // Lines or media bytes wait for a seal.
  bool pending() const;
  // The seal line for everything since the last seal, appended to the outbox. `block` is canonical
  // JSON or null; `status` a canonical JSON object or null. Returns false when nothing is pending, the
  // outbox is full, or the key would not sign (then nothing changes: the next call covers the same).
  bool seal(const char* t, bool clockSet, const char* block, const char* status);

  // The outbox: whole lines, each ending in '\n', oldest first.
  const char* outbox() const { return outbox_; }
  size_t outboxLen() const { return outboxLen_; }
  // The first `n` bytes of the outbox were sent.
  void sent(size_t n);

  uint64_t nextSeq() const { return seq_; }
  uint64_t counter() const { return ctx_ ? ctx_->counter : 0; }
  uint64_t mediaOffset() const { return offset_; }

 private:
  bool append(const char* line, size_t len);
  seal_ctx* ctx_ = nullptr;
  char* outbox_ = nullptr;
  size_t outboxCap_ = 0, outboxLen_ = 0;
  uint64_t seq_ = 1, offset_ = 0;
  bool clockUnset_ = false;  // some line since the last seal was stamped before the clock was set
};

}  // namespace DeviceSeal
