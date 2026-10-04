#include "speaker.h"
#include "config.h"

#if CAGI_SPEAKER_ENABLED
#include <deque>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include "clip.h"

namespace {
// The speaker takes the I2S port that the mic does not use. The S3 camera uses LCD_CAM, not I2S.
constexpr i2s_port_t kPort = (CAGI_AUDIO_ENABLED && !CAGI_MIC_PDM) ? I2S_NUM_0 : I2S_NUM_1;
static_assert(CAGI_SPEAKER_MAX_GAIN > 0 && CAGI_SPEAKER_MAX_GAIN <= 1.0, "CAGI_SPEAKER_MAX_GAIN must be in (0, 1]");
constexpr int32_t kGainQ15 = (int32_t)(CAGI_SPEAKER_MAX_GAIN * 32768.0);
constexpr size_t kChunk = 256;        // mono samples per i2s_write
constexpr int kDmaBufs = 6;
constexpr uint32_t kTaskStack = 40 * 1024;  // minimp3 keeps ~16 kB of scratch on the stack; TLS needs more

enum State : uint8_t { IDLE, LOADING, PLAYING };

struct Job {
  String requestId, url, hint;
  uint8_t* bytes = nullptr;
  size_t len = 0;
};

bool g_ok = false;
SemaphoreHandle_t g_lock = nullptr;  // guards every field below except the volatile timestamps
TaskHandle_t g_task = nullptr;
Job* g_pending = nullptr;
State g_state = IDLE;
bool g_enabled = true;
bool g_abort = false;
const char* g_abortReason = nullptr;
std::deque<Speaker::Result> g_results;
String g_apiHost, g_apiKey;
volatile uint32_t g_lastSoundMs = 0;
volatile bool g_everPlayed = false;
volatile bool g_playing = false;

void* psAlloc(size_t n) { return psramFound() ? ps_malloc(n) : malloc(n); }
void psFree(void* p) { free(p); }
const Clip::Allocator kPsram = {psAlloc, psFree};

void lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
void unlock() { xSemaphoreGive(g_lock); }

// Call with the lock held.
void pushResultLocked(const String& id, bool ok, uint32_t ms, const char* error) {
  Speaker::Result r;
  r.requestId = id;
  r.ok = ok;
  r.durationMs = ms;
  if (error) r.error = error;
  g_results.push_back(r);
}

void pushResult(const String& id, bool ok, uint32_t ms, const char* error) {
  lock();
  pushResultLocked(id, ok, ms, error);
  unlock();
  Serial.printf("[spk] %s %s%s%s\n", id.length() ? id.c_str() : "(no requestId)", ok ? "started" : "failed",
                error ? ": " : "", error ? error : "");
}

void freeJob(Job* j) {
  if (!j) return;
  if (j->bytes) free(j->bytes);
  delete j;
}

// Call with the lock held: drop the waiting clip and abort the current one, each with `why`.
void cancelLocked(const char* why) {
  if (g_pending) {
    pushResultLocked(g_pending->requestId, false, 0, why);
    freeJob(g_pending);
    g_pending = nullptr;
  }
  if (g_state != IDLE) {
    g_abort = true;
    g_abortReason = why;
  }
}

bool aborted() {
  lock();
  bool a = g_abort;
  unlock();
  return a;
}

const char* abortReason() {
  lock();
  const char* r = g_abortReason ? g_abortReason : "stopped";
  unlock();
  return r;
}

String hostOf(const String& url) {
  int start = url.indexOf("://");
  start = start < 0 ? 0 : start + 3;
  int end = start;
  while (end < (int)url.length() && url[end] != '/' && url[end] != ':' && url[end] != '?') end++;
  String h = url.substring(start, end);
  h.toLowerCase();
  return h;
}

void setAmp(bool on) {
  if (CAGI_SPK_SD_PIN >= 0) digitalWrite(CAGI_SPK_SD_PIN, on ? HIGH : LOW);
}

// HTTPClient::writeToStream decodes chunked bodies; this sink stops it at the byte limit or on abort.
class CapSink : public Stream {
 public:
  uint8_t* buf = nullptr;
  size_t len = 0, cap = 0;
  bool overflow = false;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* p, size_t n) override {
    if (aborted()) return 0;
    if (len + n > cap) { overflow = true; return 0; }
    memcpy(buf + len, p, n);
    len += n;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// Fetch the clip into PSRAM. Returns nullptr on success, or why it failed.
const char* fetch(Job* j) {
  if (!j->url.startsWith("https://")) return "url must be https";
  const size_t cap = CAGI_SPEAKER_MAX_BYTES;
  CapSink sink;
  sink.buf = (uint8_t*)psAlloc(cap);
  sink.cap = cap;
  if (!sink.buf) return "out of memory";
  WiFiClientSecure* tls = new WiFiClientSecure();
  tls->setInsecure();  // same trust model as cloud.cpp; the clip URL carries its own token
  const char* why = nullptr;
  {
    HTTPClient http;
    http.setTimeout(10000);
    const char* keys[] = {"Content-Type"};
    http.collectHeaders(keys, 1);
    if (!http.begin(*tls, j->url)) {
      why = "bad url";
    } else {
      // The device key goes only to the API's own host, never to a third-party clip URL.
      if (g_apiKey.length() && hostOf(j->url) == g_apiHost) http.addHeader("Authorization", "Bearer " + g_apiKey);
      const int code = http.GET();
      const int size = http.getSize();
      if (code != 200) {
        static char msg[40];
        snprintf(msg, sizeof msg, "fetch failed: HTTP %d", code);
        why = msg;
      } else if (size > (int)cap) {
        why = "clip larger than the limit";
      } else {
        String ct = http.header("Content-Type");
        if (!j->hint.length() && ct.startsWith("audio/")) j->hint = ct;
        const int n = http.writeToStream(&sink);
        if (sink.overflow) why = "clip larger than the limit";
        else if (aborted()) why = abortReason();
        else if (n < 0) why = "fetch failed: connection";
      }
      http.end();
    }
  }
  delete tls;
  if (why) {
    free(sink.buf);
    return why;
  }
  j->bytes = sink.buf;
  j->len = sink.len;
  return nullptr;
}

void playPcm(const Clip::Pcm& pcm) {
  int16_t frame[2 * kChunk];
  i2s_set_clk(kPort, pcm.rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  i2s_zero_dma_buffer(kPort);
  setAmp(true);
  vTaskDelay(pdMS_TO_TICKS(5));
  g_playing = true;
  g_everPlayed = true;
  for (size_t at = 0; at < pcm.count; at += kChunk) {
    if (aborted()) break;
    const size_t n = (pcm.count - at) < kChunk ? (pcm.count - at) : kChunk;
    for (size_t i = 0; i < n; i++) {
      const int16_t v = (int16_t)(((int32_t)pcm.samples[at + i] * kGainQ15) >> 15);
      frame[2 * i] = v;  // the same sample in both slots: the amplifier's slot choice does not matter
      frame[2 * i + 1] = v;
    }
    size_t written = 0;
    i2s_write(kPort, frame, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    g_lastSoundMs = millis();
  }
  // Let the queued DMA drain as silence before the amplifier shuts down (no click, no cut-off).
  i2s_zero_dma_buffer(kPort);
  vTaskDelay(pdMS_TO_TICKS(kDmaBufs * 256 * 1000 / pcm.rate + 10));
  setAmp(false);
  g_lastSoundMs = millis();
  g_playing = false;
}

void runJob(Job* j) {
  if (j->url.length()) {
    if (const char* why = fetch(j)) {
      pushResult(j->requestId, false, 0, why);
      return;
    }
  }
  if (aborted()) {
    pushResult(j->requestId, false, 0, abortReason());
    return;
  }
  Clip::Pcm pcm;
  const char* why = nullptr;
  if (!Clip::decode(j->bytes, j->len, j->hint.c_str(), CAGI_SPEAKER_MAX_BYTES, CAGI_SPEAKER_MAX_MS, kPsram, &pcm, &why)) {
    pushResult(j->requestId, false, 0, why);
    return;
  }
  free(j->bytes);  // the source is no longer needed; free it before playback holds the PCM
  j->bytes = nullptr;
  lock();
  const bool go = !g_abort;
  const char* reason = g_abortReason ? g_abortReason : "stopped";
  if (go) {
    g_state = PLAYING;
    pushResultLocked(j->requestId, true, pcm.durationMs(), nullptr);
  }
  unlock();
  if (!go) {
    pushResult(j->requestId, false, 0, reason);
  } else {
    Serial.printf("[spk] playing %s (%lu ms at %lu Hz)\n", j->requestId.c_str(), (unsigned long)pcm.durationMs(),
                  (unsigned long)pcm.rate);
    playPcm(pcm);
  }
  kPsram.free(pcm.samples);
}

void speakerTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      lock();
      Job* j = g_pending;
      g_pending = nullptr;
      if (j) {
        g_abort = false;
        g_abortReason = nullptr;
        g_state = LOADING;
      }
      unlock();
      if (!j) break;
      runJob(j);
      freeJob(j);
      lock();
      g_state = IDLE;
      unlock();
    }
  }
}
}  // namespace

namespace Speaker {

bool begin() {
  if (g_ok) return true;
  if (CAGI_SPK_SD_PIN >= 0) {
    pinMode(CAGI_SPK_SD_PIN, OUTPUT);
    digitalWrite(CAGI_SPK_SD_PIN, LOW);  // amplifier in shutdown until a clip plays
  }
  if (CAGI_SPK_GAIN_PIN >= 0) pinMode(CAGI_SPK_GAIN_PIN, INPUT);  // high-impedance: see config.h

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 16000;  // set per clip in playPcm()
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = kDmaBufs;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;  // an underrun sends silence, not the last buffer again

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = CAGI_SPK_BCLK_PIN;
  pins.ws_io_num = CAGI_SPK_LRCLK_PIN;
  pins.data_out_num = CAGI_SPK_DIN_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("[spk] i2s_driver_install failed");
    return false;
  }
  if (i2s_set_pin(kPort, &pins) != ESP_OK) {
    Serial.println("[spk] i2s_set_pin failed");
    i2s_driver_uninstall(kPort);
    return false;
  }
  i2s_zero_dma_buffer(kPort);
  g_lock = xSemaphoreCreateMutex();
  // Core 1 with the loop: the camera driver task and Wi-Fi run on core 0. The task sleeps in
  // i2s_write while it plays; it uses the CPU only to fetch and decode.
  if (!g_lock || xTaskCreatePinnedToCore(speakerTask, "spk", kTaskStack, nullptr, 2, &g_task, 1) != pdPASS) {
    Serial.println("[spk] task create failed");
    i2s_driver_uninstall(kPort);
    return false;
  }
  g_ok = true;
  Serial.printf("[spk] ready on I2S%d (max gain %.2f)\n", (int)kPort, (double)CAGI_SPEAKER_MAX_GAIN);
  return true;
}

bool available() { return g_ok; }

void setAuth(const String& apiBaseUrl, const String& apiKey) {
  if (!g_ok) return;
  lock();
  g_apiHost = hostOf(apiBaseUrl);
  g_apiKey = apiKey;
  unlock();
}

const char* play(Request& r) {
  const char* refuse = nullptr;
  if (!g_ok) refuse = "no speaker";
  else if (!r.url.length() && !r.bytes) refuse = "play_audio needs url or base64";
  if (refuse) {
    if (r.bytes) free(r.bytes);
    r.bytes = nullptr;
    return refuse;
  }
  lock();
  const bool busy = g_state != IDLE || g_pending;
  if (!g_enabled) refuse = "speaker output disabled by the operator";
  else if (busy && !r.interrupt) refuse = "busy: one clip at a time";
  if (!refuse) {
    if (busy) cancelLocked("interrupted by a newer clip");
    Job* j = new Job();
    j->requestId = r.requestId;
    j->url = r.url;
    j->hint = r.hint;
    j->bytes = r.bytes;
    j->len = r.len;
    g_pending = j;
  } else if (r.bytes) {
    free(r.bytes);
  }
  r.bytes = nullptr;
  unlock();
  if (!refuse) xTaskNotifyGive(g_task);
  return refuse;
}

void stop() {
  if (!g_ok) return;
  lock();
  cancelLocked("stopped");
  unlock();
}

void setEnabled(bool on) {
  if (!g_ok) return;
  lock();
  if (g_enabled != on) Serial.printf("[spk] output %s by the operator\n", on ? "enabled" : "disabled");
  g_enabled = on;
  if (!on) cancelLocked("speaker output disabled by the operator");
  unlock();
}

bool enabled() {
  if (!g_ok) return false;
  lock();
  bool e = g_enabled;
  unlock();
  return e;
}

uint32_t quietForMs() {
  if (!g_ok || !g_everPlayed) return UINT32_MAX;
  if (g_playing) return 0;
  return millis() - g_lastSoundMs;
}

bool pollResult(Result* out) {
  if (!g_ok) return false;
  lock();
  bool have = !g_results.empty();
  if (have) {
    *out = g_results.front();
    g_results.pop_front();
  }
  unlock();
  return have;
}

}  // namespace Speaker

#else  // CAGI_SPEAKER_ENABLED == 0
namespace Speaker {
bool begin() { return false; }
bool available() { return false; }
void setAuth(const String&, const String&) {}
const char* play(Request& r) {
  if (r.bytes) free(r.bytes);
  r.bytes = nullptr;
  return "no speaker";
}
void stop() {}
void setEnabled(bool) {}
bool enabled() { return false; }
uint32_t quietForMs() { return UINT32_MAX; }
bool pollResult(Result*) { return false; }
}  // namespace Speaker
#endif
