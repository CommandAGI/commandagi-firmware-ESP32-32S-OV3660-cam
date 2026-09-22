#include "audio.h"
#include "config.h"

#if CAGI_AUDIO_ENABLED
#include <driver/i2s.h>

namespace {
// CRITICAL: the ESP32 camera driver owns I2S0 for its parallel-pixel DMA. The mic MUST use I2S1 —
// installing an RX driver on I2S0 fails AND corrupts the camera's DMA, so the camera stops returning
// frames (a black "starting camera" screen). This was the root cause of cameras not streaming.
constexpr i2s_port_t kPort = I2S_NUM_1;
constexpr int kSampleRate = CAGI_AUDIO_SAMPLE_RATE;
constexpr size_t kClipSamples = (size_t)kSampleRate * CAGI_AUDIO_CLIP_MS / 1000;
constexpr size_t kPcmBytes = kClipSamples * sizeof(int16_t);
constexpr size_t kWavBytes = 44 + kPcmBytes;  // 44-byte canonical WAV header + PCM16 payload

bool g_ok = false;
uint8_t* g_wav = nullptr;   // [44-byte header | PCM16 payload], in PSRAM
int32_t* g_raw = nullptr;   // I2S scratch (32-bit slots), in PSRAM

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
}  // namespace

namespace Audio {

bool begin() {
  if (g_ok) return true;

  // Buffers live in PSRAM (the camera owns most internal RAM); fall back to heap if no PSRAM.
  g_wav = (uint8_t*)(psramFound() ? ps_malloc(kWavBytes) : malloc(kWavBytes));
  g_raw = (int32_t*)(psramFound() ? ps_malloc(kClipSamples * sizeof(int32_t)) : malloc(kClipSamples * sizeof(int32_t)));
  if (!g_wav || !g_raw) {
    Serial.println("[mic] alloc failed");
    return false;
  }
  writeWavHeader(g_wav);

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

  // Presence probe: with no INMP441 wired, the I2S data line reads as pure zeros; a real mic (even in
  // silence) dithers the low bits. If a short burst is all-zero, treat the mic as absent so we don't
  // stream a phantom silent channel — preserves the "no mic ⇒ no audio" behavior the boards expect.
  {
    bool nonzero = false;
    uint32_t t0 = millis();
    while (!nonzero && millis() - t0 < 300) {
      size_t br = 0;
      if (i2s_read(kPort, (uint8_t*)g_raw, 256 * sizeof(int32_t), &br, pdMS_TO_TICKS(100)) != ESP_OK) break;
      const size_t n = br / sizeof(int32_t);
      for (size_t i = 0; i < n; i++) if (g_raw[i] != 0) { nonzero = true; break; }
    }
    if (!nonzero) {
      Serial.println("[mic] no INMP441 detected (silent I2S) — audio disabled");
      i2s_driver_uninstall(kPort);
      return false;
    }
  }

  g_ok = true;
  Serial.printf("[mic] INMP441 ready (%d Hz, %d ms clips)\n", kSampleRate, CAGI_AUDIO_CLIP_MS);
  return true;
}

bool available() { return g_ok; }

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

  // Downconvert each left-justified 32-bit sample to PCM16 with the configured gain shift.
  int16_t* pcm = (int16_t*)(g_wav + 44);
  for (size_t i = 0; i < kClipSamples; i++) {
    int32_t v = (i < got) ? (g_raw[i] >> CAGI_AUDIO_SHIFT) : 0;
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    pcm[i] = (int16_t)v;
  }
  *buf = g_wav;
  *len = kWavBytes;
  return true;
}

void release() {}  // buffer is module-owned; nothing to free per-capture

}  // namespace Audio

#else  // CAGI_AUDIO_ENABLED == 0 — compile out the mic entirely.
namespace Audio {
bool begin() { return false; }
bool available() { return false; }
bool capture(uint8_t**, size_t*) { return false; }
void release() {}
}  // namespace Audio
#endif
