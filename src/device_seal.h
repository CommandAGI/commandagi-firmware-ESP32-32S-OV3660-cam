#pragma once
// The camera's sealed streams, without Arduino (so test/host/seal_test.cpp builds them on a PC).
//
// The device seals what it records itself (docs/integrity.md in the CommandAGI repository § sealed
// streams). Two streams, each sealed in its own records.jsonl, both with one key:
//   - `cam`: the frames. Each frame is stamped first, a JPEG COM segment `t=<ISO>` right after SOI
//     (mjpeg.js stampFrame), and those exact bytes grow its `video.mjpeg`. Its records.jsonl holds only
//     its seals, whose media roots cover video.mjpeg.
//   - `cam-at`: one line per frame sent, {"t","seq","src":"device","kind":"event","frame":{offset,length,
//     sha256}}, naming the frame's bytes in cam's video.mjpeg, and the seals over those lines.
// About once a second each stream gets a seal. The lines of each go out in order through its outbox; the
// recorder keeps them byte for byte. The seal counter is one counter for both: it always increases.
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
  uint64_t firstSeq;      // the first `cam-at` line's seq (Seal::begin: seqs never go back across reboots)
  uint64_t firstMediaSeq; // the first `cam` seal's seq
  uint64_t counter;       // the next seal's counter
};

// The bytes a stamp adds to a frame: FF FE, a 2-byte length, "t=", the time.
size_t stampOverhead(const char* t);
// SOI, the COM segment `t=<t>`, then the rest of `jpeg`. Returns the stamped length, or 0 when `jpeg`
// does not start with SOI or `out` is too small.
size_t stamp(const uint8_t* jpeg, size_t len, const char* t, uint8_t* out, size_t cap);

// Lines not yet sent: whole lines, each ending in '\n', oldest first.
struct Outbox {
  char* buf = nullptr;
  size_t cap = 0, len = 0;
  // The first `n` bytes were sent.
  void sent(size_t n);
  bool append(const char* line, size_t n);
};

class Stream {
 public:
  // `index` and `media` are the two sealers' states (about 49 kB each at the default SEAL_MAX_*), the
  // outboxes the two streams' unsent lines; the caller owns their memory.
  bool begin(const Config& cfg, seal_ctx* index, seal_ctx* media, char* indexOutbox, char* mediaOutbox, size_t outboxCap);

  // Whether a frame of `stampedLen` bytes fits in the current seals and its line in the outbox. When it
  // does not, seal first (and send the outboxes); a frame is never sent that cannot be indexed.
  bool room(size_t stampedLen) const;
  // A stamped frame that was sent: grow the media tree and write its index line. `t` is its stamp.
  // `clockSet` false makes the next seals say clock "none". False (nothing recorded) when room() is false.
  bool frameSent(const uint8_t* stamped, size_t len, const char* t, bool clockSet);

  // Lines or media bytes wait for a seal.
  bool pending() const;
  // Seal each stream that has something pending: `cam` first, then `cam-at`. `block` is canonical JSON
  // or null; `status` a canonical JSON object or null. False when a seal that was due could not be made
  // (its outbox is full, or the key would not sign); what it would have covered stays pending.
  bool seal(const char* t, bool clockSet, const char* block, const char* status);

  Outbox& indexOutbox() { return index_.out; }
  Outbox& mediaOutbox() { return media_.out; }
  uint64_t nextSeq() const { return index_.seq; }
  uint64_t nextMediaSeq() const { return media_.seq; }
  uint64_t counter() const { return counter_; }
  uint64_t mediaOffset() const { return offset_; }

 private:
  struct Part {
    seal_ctx* ctx = nullptr;
    Outbox out;
    uint64_t seq = 1;
    bool clockUnset = false;  // something since its last seal was stamped before the clock was set
    bool pending() const;
    bool seal(const char* t, bool clockSet, const char* block, const char* status, uint64_t& counter);
  };
  Part index_, media_;
  uint64_t offset_ = 0, counter_ = 0;
};

}  // namespace DeviceSeal
