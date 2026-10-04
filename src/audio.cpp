#include "audio.h"
#include "config.h"

#if CAGI_AUDIO_ENABLED
#include <driver/i2s.h>

namespace {
#if CAGI_MIC_PDM
// The ESP32-S3 has PDM RX on I2S0 only. The S3 camera driver uses LCD_CAM, not I2S (see config.h).
constexpr i2s_port_t kPort = I2S_NUM_0;
#else
// CRITICAL: the ESP32 camera driver owns I2S0 for its parallel-pixel DMA. The mic MUST use I2S1 —
// installing an RX driver on I2S0 fails AND corrupts the camera's DMA, so the camera stops returning
// frames (a black "starting camera" screen). This was the root cause of cameras not streaming.
constexpr i2s_port_t kPort = I2S_NUM_1;
#endif
constexpr int kSampleRate = CAGI_AUDIO_SAMPLE_RATE;
constexpr size_t kClipSamples = (size_t)kSampleRate * CAGI_AUDIO_CLIP_MS / 1000;
constexpr size_t kPcmBytes = kClipSamples * sizeof(int16_t);
constexpr size_t kWavBytes = 44 + kPcmBytes;  // 44-byte canonical WAV header + PCM16 payload
constexpr size_t kChunk = 256;                 // samples per i2s_read in the probe and the capture task

bool g_ok = false;
#if CAGI_MIC_PDM
const char* const kMicName = "PDM mic";
#else
const char* const kMicName = "INMP441";
#endif

void* bigAlloc(size_t n) { return psramFound() ? ps_malloc(n) : malloc(n); }

void le16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
void le32(uint8_t* p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }

// Write a canonical 44-byte PCM WAV header (mono, 16-bit) for kPcmBytes of data.
void writeWavHeader(uint8_t* h) {
  const uint32_t byteRate = (uint32_t)kSampleRate * 1 * 2;  // sampleRate * channels * bytesPerSample
  memcpy(h, "RIFF", 4);
  le32(h + 4, 36 + kPcmBytes);  // chunk size = 36 + Subchunk2Size
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  le32(h + 16, 16);             // Subchunk1Size (PCM)
  le16(h + 20, 1);             // AudioFormat = PCM
  le16(h + 22, 1);             // NumChannels = mono
  le32(h + 24, kSampleRate);
  le32(h + 28, byteRate);
  le16(h + 32, 2);             // BlockAlign = channels * bytesPerSample
  le16(h + 34, 16);            // BitsPerSample
  memcpy(h + 36, "data", 4);
  le32(h + 40, kPcmBytes);     // Subchunk2Size
}

#if CAGI_MIC_PDM
typedef int16_t RawSample;  // the PDM decimator gives PCM16 directly

bool install(i2s_channel_fmt_t slot) {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  cfg.sample_rate = kSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = slot;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_PIN_NO_CHANGE;
  pins.ws_io_num = CAGI_PDM_CLK_PIN;  // the legacy driver puts the PDM clock on the WS pin
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = CAGI_PDM_DATA_PIN;

  if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("[mic] i2s_driver_install (PDM) failed");
    return false;
  }
  if (i2s_set_pin(kPort, &pins) != ESP_OK) {
    Serial.println("[mic] i2s_set_pin (PDM) failed");
    i2s_driver_uninstall(kPort);
    return false;
  }
  return true;
}
inline int16_t toPcm(RawSample v) { return v; }
#else
typedef int32_t RawSample;  // INMP441: 24-bit data left-justified in a 32-bit slot

bool install() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = kSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;   // INMP441 is 24-bit data in a 32-bit slot
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;    // L/R tied to GND ⇒ left channel
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = CAGI_I2S_SCK_PIN;
  pins.ws_io_num = CAGI_I2S_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = CAGI_I2S_SD_PIN;

  if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("[mic] i2s_driver_install failed");
    return false;
  }
  if (i2s_set_pin(kPort, &pins) != ESP_OK) {
    Serial.println("[mic] i2s_set_pin failed");
    i2s_driver_uninstall(kPort);
    return false;
  }
  return true;
}
// Downconvert one left-justified 32-bit sample to PCM16 with the configured gain shift.
inline int16_t toPcm(RawSample s) {
  int32_t v = s >> CAGI_AUDIO_SHIFT;
  if (v > 32767) v = 32767;
  else if (v < -32768) v = -32768;
  return (int16_t)v;
}
#endif

// Presence probe: with no mic wired, the data line reads as pure zeros; a real mic (even in silence)
// dithers the low bits. If a short burst is all-zero, treat the mic as absent so we don't stream a
// phantom silent channel — preserves the "no mic ⇒ no audio" behavior the boards expect.
bool probe(RawSample* scratch) {
  uint32_t t0 = millis();
  while (millis() - t0 < 300) {
    size_t br = 0;
    if (i2s_read(kPort, (uint8_t*)scratch, kChunk * sizeof(RawSample), &br, pdMS_TO_TICKS(100)) != ESP_OK) break;
    const size_t n = br / sizeof(RawSample);
    for (size_t i = 0; i < n; i++) if (scratch[i] != 0) return true;
  }
  return false;
}

#if CAGI_AUDIO_ASYNC
// ── S3: a capture task fills a PSRAM double buffer; the main loop takes finished clips ─────────────
// Buffer states move FREE → FILLING (task) → READY (task) → POSTING (loop) → FREE (loop). g_mux
// guards every state change. With two buffers one is always the task's; if the loop still holds the
// other when a clip finishes, the task reuses the finished buffer and that clip is dropped (counted).
enum BufState : uint8_t { FREE, FILLING, READY, POSTING };
uint8_t* g_bufs[2] = {nullptr, nullptr};
volatile BufState g_state[2] = {FREE, FREE};
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool g_enabled = true;
volatile uint32_t g_dropped = 0;
RawSample* g_scratch = nullptr;  // one i2s_read chunk, internal RAM
TaskHandle_t g_task = nullptr;

int acquireFill(int justFilled) {
  int pick = -1;
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < 2 && pick < 0; i++) if (g_state[i] == FREE) pick = i;
  if (pick < 0) {
    pick = justFilled;  // the other buffer is POSTING: drop the clip we just finished
    g_dropped = g_dropped + 1;
  }
  g_state[pick] = FILLING;
  portEXIT_CRITICAL(&g_mux);
  return pick;
}

void publish(int i) {
  portENTER_CRITICAL(&g_mux);
  g_state[i] = READY;
  portEXIT_CRITICAL(&g_mux);
}

void dropReady() {
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < 2; i++) if (g_state[i] == READY) g_state[i] = FREE;
  portEXIT_CRITICAL(&g_mux);
}

void captureTask(void*) {
  int fill = acquireFill(-1);
  size_t got = 0;
  bool running = true;
  uint32_t droppedSeen = 0;
  for (;;) {
    if (!g_enabled) {
      // Operator turned the mic off: stop the I2S clock, so the mic is not sampled at all.
      if (running) { i2s_stop(kPort); running = false; }
      got = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (!running) { i2s_start(kPort); running = true; got = 0; }
    size_t br = 0;
    const size_t want = (kClipSamples - got) < kChunk ? (kClipSamples - got) : kChunk;
    if (i2s_read(kPort, (uint8_t*)g_scratch, want * sizeof(RawSample), &br, pdMS_TO_TICKS(200)) != ESP_OK || br == 0)
      continue;
    const size_t n = br / sizeof(RawSample);
    int16_t* pcm = (int16_t*)(g_bufs[fill] + 44) + got;
    for (size_t i = 0; i < n; i++) pcm[i] = toPcm(g_scratch[i]);
    got += n;
    if (got >= kClipSamples) {
      publish(fill);
      fill = acquireFill(fill);
      got = 0;
      if (g_dropped != droppedSeen) {
        droppedSeen = g_dropped;
        Serial.printf("[mic] the loop was still posting; dropped a clip (total %lu)\n", (unsigned long)droppedSeen);
      }
    }
  }
}
#else
// ── Classic ESP32: blocking capture of one clip, unchanged ───────────────────────────────────────
uint8_t* g_wav = nullptr;   // [44-byte header | PCM16 payload], in PSRAM
int32_t* g_raw = nullptr;   // I2S scratch (32-bit slots), in PSRAM
#endif
}  // namespace

namespace Audio {

bool begin() {
  if (g_ok) return true;

#if CAGI_AUDIO_ASYNC
  // Buffers live in PSRAM (the camera owns most internal RAM); fall back to heap if no PSRAM.
  g_bufs[0] = (uint8_t*)bigAlloc(kWavBytes);
  g_bufs[1] = (uint8_t*)bigAlloc(kWavBytes);
  g_scratch = (RawSample*)malloc(kChunk * sizeof(RawSample));
  if (!g_bufs[0] || !g_bufs[1] || !g_scratch) {
    Serial.println("[mic] alloc failed");
    return false;
  }
  writeWavHeader(g_bufs[0]);
  writeWavHeader(g_bufs[1]);
  RawSample* scratch = g_scratch;
#else
  // Buffers live in PSRAM (the camera owns most internal RAM); fall back to heap if no PSRAM.
  g_wav = (uint8_t*)bigAlloc(kWavBytes);
  g_raw = (int32_t*)bigAlloc(kClipSamples * sizeof(int32_t));
  if (!g_wav || !g_raw) {
    Serial.println("[mic] alloc failed");
    return false;
  }
  writeWavHeader(g_wav);
  RawSample* scratch = g_raw;
#endif

#if CAGI_MIC_PDM
  // The mic's L/R pin is tied to GND (DFR1154 R3). Which legacy-driver slot that maps to is not
  // proven on this board yet, so probe the left slot and fall back to the right one.
  bool found = false;
  const i2s_channel_fmt_t slots[2] = {I2S_CHANNEL_FMT_ONLY_LEFT, I2S_CHANNEL_FMT_ONLY_RIGHT};
  for (int i = 0; i < 2 && !found; i++) {
    if (!install(slots[i])) return false;
    found = probe(scratch);
    if (!found) i2s_driver_uninstall(kPort);
    else Serial.printf("[mic] PDM data on the %s slot\n", i == 0 ? "left" : "right");
  }
  if (!found) {
    Serial.println("[mic] PDM mic silent on both slots — audio disabled");
    return false;
  }
#else
  if (!install()) return false;
  if (!probe(scratch)) {
    Serial.println("[mic] no INMP441 detected (silent I2S) — audio disabled");
    i2s_driver_uninstall(kPort);
    return false;
  }
#endif

#if CAGI_AUDIO_ASYNC
  // Pinned to the Arduino loop's core (1). The camera driver's task and the Wi-Fi stack run on core 0
  // (CONFIG_CAMERA_CORE0); this task sleeps in i2s_read almost all the time, so the frame loop on core
  // 1 keeps its rate. Priority 2 > loop (1), so a clip never waits behind a long loop pass.
  if (xTaskCreatePinnedToCore(captureTask, "mic", 4096, nullptr, 2, &g_task, 1) != pdPASS) {
    Serial.println("[mic] capture task create failed");
    i2s_driver_uninstall(kPort);
    return false;
  }
#endif

  g_ok = true;
  Serial.printf("[mic] %s ready (%d Hz, %d ms clips%s)\n", kMicName, kSampleRate, CAGI_AUDIO_CLIP_MS,
                CAGI_AUDIO_ASYNC ? ", capture task" : "");
  return true;
}

bool available() { return g_ok; }

#if CAGI_AUDIO_ASYNC
void setEnabled(bool on) {
  if (!g_ok || g_enabled == on) return;
  g_enabled = on;
  if (!on) dropReady();  // a clip recorded before the mic was turned off is never posted
}

bool capture(uint8_t** buf, size_t* len) {
  if (!g_ok || !g_enabled) return false;
  int pick = -1;
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < 2 && pick < 0; i++) if (g_state[i] == READY) pick = i;
  if (pick >= 0) g_state[pick] = POSTING;
  portEXIT_CRITICAL(&g_mux);
  if (pick < 0) return false;
  *buf = g_bufs[pick];
  *len = kWavBytes;
  return true;
}

void release() {
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < 2; i++) if (g_state[i] == POSTING) g_state[i] = FREE;
  portEXIT_CRITICAL(&g_mux);
}
#else
void setEnabled(bool) {}  // the classic path captures only when the loop asks

bool capture(uint8_t** buf, size_t* len) {
  if (!g_ok) return false;
  size_t got = 0;
  while (got < kClipSamples) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(kPort, (uint8_t*)(g_raw + got), (kClipSamples - got) * sizeof(int32_t),
                             &bytesRead, pdMS_TO_TICKS(CAGI_AUDIO_CLIP_MS + 200));
    if (err != ESP_OK || bytesRead == 0) break;
    got += bytesRead / sizeof(int32_t);
  }
  if (got == 0) return false;

  int16_t* pcm = (int16_t*)(g_wav + 44);
  for (size_t i = 0; i < kClipSamples; i++) pcm[i] = (i < got) ? toPcm(g_raw[i]) : 0;
  *buf = g_wav;
  *len = kWavBytes;
  return true;
}

void release() {}  // buffer is module-owned; nothing to free per-capture
#endif

}  // namespace Audio

#else  // CAGI_AUDIO_ENABLED == 0 — compile out the mic entirely.
namespace Audio {
bool begin() { return false; }
bool available() { return false; }
void setEnabled(bool) {}
bool capture(uint8_t**, size_t*) { return false; }
void release() {}
}  // namespace Audio
#endif
