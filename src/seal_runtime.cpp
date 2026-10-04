#include "seal_runtime.h"

#if CAGI_DEVICE_SEALS
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_flash_encrypt.h>
#include <esp_secure_boot.h>
#include <sys/time.h>
#include <time.h>
#include <sodium.h>
#include "mbedtls/base64.h"
#include "device_seal.h"
#include "camera.h"
#include "cloud.h"
#include "cloud_ws.h"

namespace {
// Seqs and seal counters must never go back, also across a reboot, but NVS cannot take a write per
// line. So a block of each is reserved in NVS ahead of use; a reboot skips the rest of the block.
constexpr uint64_t kSeqBlock = 4096;
constexpr uint64_t kCounterBlock = 1024;
constexpr size_t kOutboxBytes = 32 * 1024;  // each stream's
constexpr uint32_t kSealEveryMs = 1000;
constexpr uint32_t kHeadEveryMs = 10000;
// Times before this were not set by SNTP: the RTC starts at 1970 on every boot and SNTP is the only clock.
constexpr time_t kClockSetAfter = 1735689600;  // 2025-01-01T00:00:00Z

bool g_ready = false;
uint8_t g_pk[32];
uint8_t g_sk[64];
uint8_t g_spki[44] = {0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00};
char g_spkiB64u[64];
String g_chain;  // JSON array of base64 DER certificates, leaf first; empty until the factory certifies the key
String g_announce;
char g_status[96];
String g_log;  // canonical {"head","seq"} of the contract log's last head fetched; empty for none
// A frame request waiting for serveFrameRequest: one at a time (a second one is refused at once).
struct FrameRequest {
  bool waiting = false;
  char requestId[97];
  char channelId[97];
};
FrameRequest g_frameRequest;
uint64_t g_seqLimit = 0, g_mediaSeqLimit = 0, g_counterLimit = 0;
uint32_t g_nextSeal = 0, g_nextHead = 0;
bool g_sntpStarted = false;
seal_ctx* g_index = nullptr;  // cam-at's sealer
seal_ctx* g_media = nullptr;  // cam's sealer
char* g_indexOut = nullptr;
char* g_mediaOut = nullptr;
uint8_t* g_stamped = nullptr;
size_t g_stampedCap = 0;
DeviceSeal::Stream g_stream;
char g_line[4096];
size_t g_lineLen = 0;

int signEd25519(void*, const uint8_t* msg, size_t len, uint8_t sig[64]) {
  return crypto_sign_detached(sig, nullptr, msg, len, g_sk) == 0 ? 0 : -1;
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

void* psram(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}

bool clockSet() { return time(nullptr) > kClockSetAfter; }

void isoNow(char out[64]) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tm;
  gmtime_r(&tv.tv_sec, &tm);
  snprintf(out, 64, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
           tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
}

// Reserve the next block of `key` when `next` comes near the reserved limit.
void reserve(const char* key, uint64_t next, uint64_t& limit, uint64_t block) {
  if (next + 64 < limit) return;
  limit = next + block;
  Preferences p;
  if (p.begin("cagi-seal", false)) {
    p.putULong64(key, limit);
    p.end();
  }
}

// Each stream's lines go to its own channel: cam's seals to `cam`, the index and its seals to `cam-at`.
// The index goes out only after the seal over its frames: the recorder then never keeps an index line
// whose frame it refused (thread-run.ts resumes both streams when it refuses either).
void flush() {
  if (!CloudWs::connected()) return;
  DeviceSeal::Outbox& m = g_stream.mediaOutbox();
  if (m.len && CloudWs::sendLines(CAGI_STREAM_CHANNEL, "camera", m.buf, m.len)) m.sent(m.len);
  if (m.len) return;
  DeviceSeal::Outbox& i = g_stream.indexOutbox();
  if (i.len && CloudWs::sendLines(CAGI_SEAL_INDEX_CHANNEL, "events", i.buf, i.len)) i.sent(i.len);
}

void reserveAll() {
  reserve("seq", g_stream.nextSeq(), g_seqLimit, kSeqBlock);
  reserve("mseq", g_stream.nextMediaSeq(), g_mediaSeqLimit, kSeqBlock);
  reserve("ctr", g_stream.counter(), g_counterLimit, kCounterBlock);
}

void sealNow() {
  char t[64];
  isoNow(t);
  if (!g_stream.seal(t, clockSet(), g_log.length() ? g_log.c_str() : nullptr, g_status))
    Serial.println("[seal] could not seal (an outbox is full, or the key would not sign)");
  reserveAll();
  flush();
}

// The base64 bodies of a JSON array of strings, checked: every item decodes, and the leaf holds our key.
bool chainHoldsOurKey(const String& json) {
  JsonDocument d;
  if (deserializeJson(d, json) || !d.is<JsonArray>() || d.as<JsonArray>().size() == 0) return false;
  for (JsonVariant v : d.as<JsonArray>()) {
    if (!v.is<const char*>()) return false;
  }
  const char* leaf = d[0];
  size_t n = 0;
  if (mbedtls_base64_decode(nullptr, 0, &n, (const uint8_t*)leaf, strlen(leaf)) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || n == 0) return false;
  uint8_t* der = (uint8_t*)psram(n);
  if (!der) return false;
  bool ok = mbedtls_base64_decode(der, n, &n, (const uint8_t*)leaf, strlen(leaf)) == 0 &&
            memmem(der, n, g_spki, sizeof g_spki) != nullptr;
  free(der);
  return ok;
}

void command(const char* line) {
  if (!strcmp(line, "seal-spki")) {
    char b64[80];
    size_t n = 0;
    mbedtls_base64_encode((uint8_t*)b64, sizeof b64, &n, g_spki, sizeof g_spki);
    b64[n] = 0;
    Serial.printf("-----BEGIN PUBLIC KEY-----\n%s\n-----END PUBLIC KEY-----\n", b64);
  } else if (!strncmp(line, "seal-chain ", 11)) {
    const String chain = String(line + 11);
    if (!chainHoldsOurKey(chain)) {
      Serial.println("seal-chain refused: not a JSON array of base64 certificates whose leaf holds this key");
      return;
    }
    Preferences p;
    if (!p.begin("cagi-seal", false) || !p.putString("chain", chain)) {
      Serial.println("seal-chain refused: NVS write failed");
      return;
    }
    p.end();
    g_chain = chain;
    Serial.println("seal-chain ok: the first seal after the next boot announces it");
  } else if (!strcmp(line, "seal-chain-clear")) {
    Preferences p;
    if (p.begin("cagi-seal", false)) {
      p.remove("chain");
      p.end();
    }
    g_chain = "";
    Serial.println("seal-chain cleared: the first seal after the next boot announces the bare key");
  } else if (!strcmp(line, "seal-status")) {
    Serial.printf("seal-status key=%s certified=%s seq=%llu/%llu counter=%llu clock=%s log=%s status=%s\n", g_index ? g_index->key : "none",
                  g_chain.length() ? "yes" : "no", (unsigned long long)g_stream.nextSeq(), (unsigned long long)g_stream.nextMediaSeq(),
                  (unsigned long long)g_stream.counter(), clockSet() ? "ntp" : "none", g_log.length() ? g_log.c_str() : "none",
                  g_status);
  }
}
}  // namespace

namespace Seal {

void begin() {
  if (g_ready) return;
  if (sodium_init() < 0) {
    Serial.println("[seal] libsodium did not start: this unit does not seal");
    return;
  }
  Preferences p;
  if (!p.begin("cagi-seal", false)) {
    Serial.println("[seal] NVS namespace cagi-seal did not open: this unit does not seal");
    return;
  }
  uint8_t seed[32];
  if (p.getBytesLength("seed") == sizeof seed) {
    p.getBytes("seed", seed, sizeof seed);
  } else {
    esp_fill_random(seed, sizeof seed);
    if (p.putBytes("seed", seed, sizeof seed) != sizeof seed) {
      p.end();
      memset(seed, 0, sizeof seed);
      Serial.println("[seal] could not store a new key: this unit does not seal");
      return;
    }
    Serial.println("[seal] made this unit's key (first boot)");
  }
  crypto_sign_seed_keypair(g_pk, g_sk, seed);
  memset(seed, 0, sizeof seed);
  g_chain = p.getString("chain", "");
  const uint64_t firstSeq = p.getULong64("seq", 1);
  const uint64_t firstMediaSeq = p.getULong64("mseq", 1);
  const uint64_t counter = p.getULong64("ctr", 0);
  p.end();

  memcpy(g_spki + 12, g_pk, 32);
  b64u(g_spki, sizeof g_spki, g_spkiB64u);
  g_announce = g_chain.length() ? g_chain : String(g_spkiB64u);
  // What the device says about itself in each seal: signed, so not forged in transit; still a claim.
  snprintf(g_status, sizeof g_status, "{\"boot\":\"%s\",\"flash\":\"%s\",\"fw\":\"%s\"}",
           esp_secure_boot_enabled() ? "verified" : "unverified", esp_flash_encryption_enabled() ? "encrypted" : "plain",
           CAGI_FW_VERSION);

  g_index = (seal_ctx*)psram(sizeof(seal_ctx));
  g_media = (seal_ctx*)psram(sizeof(seal_ctx));
  g_indexOut = (char*)psram(kOutboxBytes);
  g_mediaOut = (char*)psram(kOutboxBytes);
  const DeviceSeal::Config cfg = {"ed25519", g_spki, sizeof g_spki, g_announce.c_str(), g_chain.length() > 0, signEd25519, nullptr,
                                  firstSeq, firstMediaSeq, counter};
  if (!g_index || !g_media || !g_indexOut || !g_mediaOut || !g_stream.begin(cfg, g_index, g_media, g_indexOut, g_mediaOut, kOutboxBytes)) {
    Serial.println("[seal] out of memory: this unit does not seal");
    return;
  }
  // This boot's blocks start where the last boot's reservations ended.
  reserveAll();
  g_ready = true;
  Serial.printf("[seal] key %s (%s), seqs from %llu/%llu, %s\n", g_index->key, g_chain.length() ? "certified" : "not certified",
                (unsigned long long)firstSeq, (unsigned long long)firstMediaSeq, g_status);
}

void serial() {
  while (Serial.available()) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      g_line[g_lineLen] = 0;
      if (g_lineLen && g_ready) command(g_line);
      g_lineLen = 0;
    } else if (g_lineLen + 1 < sizeof g_line) {
      g_line[g_lineLen++] = (char)c;
    }
  }
}

bool sendFrame(const uint8_t* jpeg, size_t len) {
  if (!g_ready) return false;
  char t[64];
  isoNow(t);
  const size_t need = len + DeviceSeal::stampOverhead(t);
  if (need > g_stampedCap) {
    free(g_stamped);
    g_stampedCap = need + 16 * 1024;
    g_stamped = (uint8_t*)psram(g_stampedCap);
    if (!g_stamped) {
      g_stampedCap = 0;
      return false;
    }
  }
  if (!g_stream.room(need)) sealNow();
  if (!g_stream.room(need)) return false;  // the outbox is still full: the socket is not taking lines
  const size_t n = DeviceSeal::stamp(jpeg, len, t, g_stamped, g_stampedCap);
  if (!n || !CloudWs::sendFrameJson(CAGI_STREAM_CHANNEL, g_stamped, n)) return false;
  g_stream.frameSent(g_stamped, n, t, clockSet());
  reserveAll();
  return true;
}

void loop(const Creds& c) {
  if (!g_ready) return;
  const uint32_t now = millis();
  if (!g_sntpStarted && CloudWs::connected()) {
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    g_sntpStarted = true;
  }
  if (CloudWs::connected() && (int32_t)(now - g_nextHead) >= 0) {
    String head;
    if (Cloud::logHead(c, head)) g_log = head;
    g_nextHead = millis() + kHeadEveryMs;
  }
  // Lines go out once a second, with their seal, not one message per frame.
  if ((int32_t)(now - g_nextSeal) >= 0) {
    if (g_stream.pending()) sealNow();
    else flush();
    g_nextSeal = millis() + kSealEveryMs;
  }
}

String spki() { return g_ready ? String(g_spkiB64u) : String(); }

namespace {
void answerFrame(const char* requestId, const char* channelId, uint64_t seq, const char* hash, const char* refused) {
  char out[320];
  if (DeviceSeal::frameAnswer(out, sizeof out, requestId, channelId, seq, hash, refused)) CloudWs::sendText(String(out));
}
}  // namespace

void serveFrameRequest(bool cameraOn) {
  if (!g_frameRequest.waiting) return;
  FrameRequest r = g_frameRequest;
  g_frameRequest.waiting = false;
  if (!g_ready) return answerFrame(r.requestId, r.channelId, 0, nullptr, "this unit does not seal");
  if (!cameraOn) return answerFrame(r.requestId, r.channelId, 0, nullptr, "the camera is off");
  uint8_t* buf = nullptr;
  size_t len = 0;
  if (!Camera::capture(&buf, &len)) return answerFrame(r.requestId, r.channelId, 0, nullptr, "the sensor returned no frame");
  const bool sent = sendFrame(buf, len);
  Camera::release();
  if (!sent) return answerFrame(r.requestId, r.channelId, 0, nullptr, "the frame did not go out (the socket is not taking it)");
  // Seal now, not at the next second: the answer names the seal over this frame, and the lines go out first.
  sealNow();
  g_nextSeal = millis() + kSealEveryMs;
  if (g_stream.mediaOutbox().len || g_stream.indexOutbox().len)
    return answerFrame(r.requestId, r.channelId, 0, nullptr, "captured and sealed, but the seal lines did not go out yet");
  uint64_t seq = 0;
  const char* hash = nullptr;
  if (!g_stream.lastMediaSeal(seq, hash)) return answerFrame(r.requestId, r.channelId, 0, nullptr, "no seal was made");
  answerFrame(r.requestId, r.channelId, seq, hash, nullptr);
  Serial.printf("[seal] frame request %s: seal %llu\n", r.requestId, (unsigned long long)seq);
}

bool onMessage(const uint8_t* payload, size_t len) {
  static const char kType[] = "\"seal_resume\"";
  static const char kFrame[] = "\"frame_request\"";
  const bool resume = memmem(payload, len, kType, sizeof kType - 1) != nullptr;
  if (!resume && !memmem(payload, len, kFrame, sizeof kFrame - 1)) return false;
  JsonDocument msg;
  if (deserializeJson(msg, payload, len)) return false;
  if (!strcmp(msg["type"] | "", "frame_request")) {
    const char* id = msg["requestId"] | "";
    const char* ch = msg["channelId"] | "";
    if (strcmp(ch, CAGI_STREAM_CHANNEL)) answerFrame(id, ch, 0, nullptr, "this channel takes no frame request");
    else if (g_frameRequest.waiting) answerFrame(id, ch, 0, nullptr, "another frame request is waiting");
    else if (strlen(id) < sizeof g_frameRequest.requestId) {
      strcpy(g_frameRequest.requestId, id);
      strcpy(g_frameRequest.channelId, ch);
      g_frameRequest.waiting = true;
    }
    return true;
  }
  if (strcmp(msg["type"] | "", "seal_resume")) return false;
  if (!g_ready) return true;
  // The hashes must outlive the JsonDocument's strings only until resume() copies them.
  DeviceSeal::FileState index{}, media{};
  bool hasIndex = false, hasMedia = false;
  int64_t lastCounter = -1;
  JsonDocument reply;
  reply["type"] = "seal_resumed";
  JsonArray ids = reply["channelIds"].to<JsonArray>();
  for (JsonObjectConst x : msg["streams"].as<JsonArrayConst>()) {
    const char* id = x["channelId"] | "";
    DeviceSeal::FileState f{x["seq"] | (uint64_t)0, x["prev"].is<const char*>() ? x["prev"].as<const char*>() : nullptr,
                            x["bytes"] | (uint64_t)0};
    if (!strcmp(id, CAGI_SEAL_INDEX_CHANNEL)) {
      index = f;
      hasIndex = true;
    } else if (!strcmp(id, CAGI_STREAM_CHANNEL)) {
      media = f;
      hasMedia = true;
    } else {
      continue;
    }
    if (x["counter"].is<int64_t>() && x["counter"].as<int64_t>() > lastCounter) lastCounter = x["counter"].as<int64_t>();
    ids.add(id);
  }
  g_stream.resume(hasIndex ? &index : nullptr, hasMedia ? &media : nullptr, lastCounter);
  reserveAll();
  String out;
  serializeJson(reply, out);
  CloudWs::sendText(out);
  Serial.printf("[seal] resumed where the recorder stands: %s\n", msg["reason"] | "");
  return true;
}

}  // namespace Seal
#endif
