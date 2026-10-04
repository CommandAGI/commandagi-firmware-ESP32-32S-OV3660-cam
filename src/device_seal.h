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
// recorder keeps them byte for byte. The seal counter is one counter for both: it always increases. Each
// seal names the contract log's head (`log`), so it was made after the block the head descends from.
// A frame request (`frame_request`, README § Sealed stream) seals both streams at once, not at the next
// second, and is answered with cam's seal over the frame (frameAnswer).
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

// The answer to a frame request, one JSON line without a newline (frame-requests.js parseFrameSealed):
// {"type":"frame_sealed","requestId","channelId","seq","hash"} for cam's seal over the frame, or, with
// `refused` set, {"type":"frame_sealed","requestId","channelId","refused"}. Returns its length, or 0 when
// an id is not [A-Za-z0-9._:-]{1,96}, `refused` holds a quote, a backslash or a control character, or
// `out` is too small.
size_t frameAnswer(char* out, size_t cap, const char* requestId, const char* channelId, uint64_t seq, const char* hash,
                   const char* refused);

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

// Where the recorder's file of one stream stands (a `seal_resume`): its last seq, its last seal's sha256
// (hex; null when the file has none), and for `cam` how many bytes video.mjpeg holds.
struct FileState {
  uint64_t seq;
  const char* prev;
  uint64_t bytes;
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

  // The recorder did not keep what was sent (a frame lost on the way, a reboot, a reconnect) and says
  // where its files stand. Everything not yet sealed and kept is dropped from both streams, because one
  // describes the other (an index line names its frame's bytes): the pending leaves and both outboxes.
  // A stream named continues after its file: its next seq after the file's last, its next seal chained
  // to the file's last seal and announcing the key again, `cam`'s frames at the file's length. Seqs and
  // the counter never go back. `lastCounter` < 0: the files have no seal yet.
  void resume(const FileState* index, const FileState* media, int64_t lastCounter);
  // Seal each stream that has something pending: `cam` first, then `cam-at`. `log` is the contract log's
  // head as canonical JSON ({"head","seq"}) or null; `status` a canonical JSON object or null. False when a
  // seal that was due could not be made (its outbox is full, or the key would not sign); what it would have
  // covered stays pending.
  bool seal(const char* t, bool clockSet, const char* log, const char* status);
  // cam's last seal: its seq and the sha256 (hex) of its line. False before the first.
  bool lastMediaSeal(uint64_t& seq, const char*& hash) const;

  Outbox& indexOutbox() { return index_.out; }
  Outbox& mediaOutbox() { return media_.out; }
  uint64_t nextSeq() const { return index_.seq; }
  uint64_t nextMediaSeq() const { return media_.seq; }
  uint64_t counter() const { return counter_; }
  uint64_t mediaOffset() const { return offset_; }

 private:
  struct Part {
    seal_ctx* ctx = nullptr;
    // Drop what is pending; with `file`, continue after it.
    void reset(const Config& cfg, const FileState* file);
    Outbox out;
    uint64_t seq = 1;
    bool clockUnset = false;  // something since its last seal was stamped before the clock was set
    bool pending() const;
    bool seal(const char* t, bool clockSet, const char* log, const char* status, uint64_t& counter);
  };
  Part index_, media_;
  Config cfg_{};
  uint64_t offset_ = 0, counter_ = 0;
};

}  // namespace DeviceSeal
