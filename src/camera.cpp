#include "camera.h"
#include "esp_camera.h"
#include "config.h"

namespace {
camera_fb_t* g_fb = nullptr;
}

namespace Camera {

bool begin() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  // XCLK per board (see CAGI_XCLK_HZ in config.h): 16 MHz is a widely-reported fps jump on OV2640
  // clones; the OV3660 (DFRobot S3 AI Camera) needs its standard 20 MHz or it inits but never emits a
  // frame. QVGA keeps each JPEG ~10-20KB so the WS stream can push ~30fps within the Wi-Fi budget.
  config.xclk_freq_hz = CAGI_XCLK_HZ;
  config.frame_size = FRAMESIZE_QVGA;      // 320x240 — sized for high-frame-rate MJPEG streaming
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;   // always serve the freshest frame
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;                // 0..63, lower = better quality / bigger
  config.fb_count = 2;                      // double-buffer: the sensor fills the next frame in PSRAM
                                            // while we upload the current one — smoother at 5fps. PSRAM
                                            // has the room; BLE is stopped while streaming anyway.

  if (!psramFound()) {
    // No PSRAM: fall back to a smaller frame in internal RAM so we still work.
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed: 0x%x\n", err);
    return false;
  }
  return true;
}

bool capture(uint8_t** buf, size_t* len) {
  g_fb = esp_camera_fb_get();
  if (!g_fb || g_fb->len == 0) {
    if (g_fb) { esp_camera_fb_return(g_fb); g_fb = nullptr; }
    return false;
  }
  *buf = g_fb->buf;
  *len = g_fb->len;
  return true;
}

void release() {
  if (g_fb) {
    esp_camera_fb_return(g_fb);
    g_fb = nullptr;
  }
}

// Tear the sensor down and bring it back up — recovery for a wedged OV2640 that stops returning
// frames at runtime (brownout / lost SCCB sync). Returns true if it re-initialized.
bool reinit() {
  release();
  esp_camera_deinit();
  delay(50);
  return begin();
}

// ── Hardware self-test (CAGI_CAMTEST builds) ─────────────────────────────────────────────────────
// Decide "the sensor is damaged" vs "config/power". Two independent signals:
//   1) SCCB chip-ID read (PID/VER over the I2C control bus) — proves the OV2640 silicon is alive and
//      talking, independent of the parallel pixel bus. PID 0x26 = OV2640.
//   2) Actual frame grabs across several XCLK freqs + frame sizes — proves the parallel data path
//      (PCLK/VSYNC/HREF/D0-D7) is delivering pixels. Lower XCLK (10MHz) often rescues flaky clones.
// A clean ID read + zero frames anywhere points at the data bus / ribbon / power, not dead silicon.
static bool tryConfig(int xclkHz, framesize_t fs, const char* label) {
  esp_camera_deinit();
  delay(60);
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM; config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = xclkHz;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = fs;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  Serial.printf("[camtest] %-18s xclk=%2dMHz init=%s\n", label, xclkHz / 1000000,
                err == ESP_OK ? "OK" : "");
  if (err != ESP_OK) { Serial.printf("           init FAILED err=0x%x (%s)\n", err, esp_err_to_name(err)); return false; }

  sensor_t* s = esp_camera_sensor_get();
  if (s) Serial.printf("           sensor PID=0x%02x VER=0x%02x MIDH=0x%02x MIDL=0x%02x %s\n",
                       s->id.PID, s->id.VER, s->id.MIDH, s->id.MIDL,
                       s->id.PID == 0x26 ? "(OV2640 ✓)" : "(unexpected!)");
  else Serial.println("           sensor_get() returned NULL");

  int got = 0;
  for (int i = 0; i < 5; i++) {
    uint32_t t0 = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    uint32_t dt = millis() - t0;
    if (fb && fb->len > 0) {
      got++;
      Serial.printf("           frame %d: %u bytes %ux%u (%lums)\n", i, (unsigned)fb->len,
                    (unsigned)fb->width, (unsigned)fb->height, (unsigned long)dt);
      esp_camera_fb_return(fb);
    } else {
      if (fb) esp_camera_fb_return(fb);
      Serial.printf("           frame %d: NO FRAME (%lums)\n", i, (unsigned long)dt);
    }
    delay(200);
  }
  Serial.printf("           => %d/5 frames\n", got);
  return got > 0;
}

void selftest() {
  Serial.println("\n[camtest] ===== OV2640 self-test =====");
  Serial.printf("[camtest] PSRAM: %s\n", psramFound() ? "yes" : "no");
  struct { int hz; framesize_t fs; const char* label; } combos[] = {
    {20000000, FRAMESIZE_VGA,   "VGA@20"},
    {20000000, FRAMESIZE_QVGA,  "QVGA@20"},
    {16000000, FRAMESIZE_QVGA,  "QVGA@16"},
    {10000000, FRAMESIZE_QVGA,  "QVGA@10"},
    {10000000, FRAMESIZE_QQVGA, "QQVGA@10"},
    { 8000000, FRAMESIZE_QQVGA, "QQVGA@8"},
  };
  int worked = 0;
  for (auto& c : combos) if (tryConfig(c.hz, c.fs, c.label)) worked++;
  esp_camera_deinit();
  Serial.printf("[camtest] ===== done: %d/%d configs produced frames =====\n", worked, (int)(sizeof(combos)/sizeof(combos[0])));
  if (worked == 0) Serial.println("[camtest] VERDICT: no frames at any config. If a sensor PID read OK above, the chip is\n"
                                  "          ALIVE — suspect the ribbon/data pins/power, not dead silicon.");
}

}  // namespace Camera
