#include "ota.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include "mbedtls/sha256.h"
#include "mbedtls/version.h"
#include "config.h"
#include "cloud_ws.h"
#include "ota_wire.h"

#ifndef CAGI_OTA_CONFIRM_MS
#define CAGI_OTA_CONFIRM_MS 300000
#endif

// arduino-esp32 marks a new image valid as soon as it starts unless this says the app decides later
// (cores/esp32/esp32-hal-misc.c, under CONFIG_APP_ROLLBACK_ENABLE, which its sdkconfig sets with
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). This app decides: when its socket reaches the host (Ota::onConnected).
extern "C" bool verifyRollbackLater() { return true; }

namespace {
// Per slot (partition label): f_<label> the image file's SHA-256 as written, p_<label> the partition digest right
// after, v_<label> the version the update named. pend: the slot an update was written to, until it confirms or goes back.
const char* kNs = "cagi_ota";

enum class Job { None, Update, Rollback };
Job g_job = Job::None;
bool g_busy = false;
bool g_report = false;
bool g_confirming = false;
uint32_t g_confirmBy = 0;
uint32_t g_boots = 0;
String g_bootReply;  // said after the first report of this boot (a rollback at boot, a confirmation)
struct {
  char sha[65];
  long long size;
  char version[32];
  char path[244];
} g_update;

String key(char kind, const char* label) { return String(kind) + "_" + label; }

OtaWire::SlotRecord recordOf(const char* label) {
  OtaWire::SlotRecord r;
  Preferences p;
  if (p.begin(kNs, true)) {
    p.getString(key('f', label).c_str(), r.fileSha, sizeof r.fileSha);
    p.getString(key('p', label).c_str(), r.partitionSha, sizeof r.partitionSha);
    p.getString(key('v', label).c_str(), r.version, sizeof r.version);
    p.end();
  }
  return r;
}

bool partitionSha(const esp_partition_t* part, char out[65]) {
  uint8_t d[32];
  if (!part || esp_partition_get_sha256(part, d) != ESP_OK) return false;
  OtaWire::toHex(d, out);
  return true;
}

// The image file a slot holds, when the device wrote it and the slot is unchanged since; else nullptr.
const char* fileShaOf(const esp_partition_t* part, OtaWire::SlotRecord& r, char now[65]) {
  r = recordOf(part->label);
  return partitionSha(part, now) ? OtaWire::fileShaOf(r, now) : nullptr;
}

String pending() {
  Preferences p;
  String s;
  if (p.begin(kNs, true)) {
    s = p.getString("pend", "");
    p.end();
  }
  return s;
}

void setPending(const char* label) {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  if (label && *label) p.putString("pend", label);
  else p.remove("pend");
  p.end();
}

void send(JsonDocument& d) {
  String out;
  serializeJson(d, out);
  CloudWs::sendText(out);
}

void reply(const String& text) {
  JsonDocument d;
  d["type"] = "firmware_reply";
  d["text"] = text;
  send(d);
  Serial.printf("[ota] %s\n", text.c_str());
}

// What runs, as the device can prove it: the file digest only when this device wrote the slot and it is unchanged.
void state(const char* status) {
  JsonDocument d;
  d["type"] = "firmware_state";
  d["status"] = status;
  const esp_partition_t* run = esp_ota_get_running_partition();
  JsonObject r = d["running"].to<JsonObject>();
  r["version"] = CAGI_FW_VERSION;
  OtaWire::SlotRecord rec;
  char now[65] = "";
  if (run) {
    r["slot"] = run->label;
    if (const char* f = fileShaOf(run, rec, now)) r["sha256"] = f;
    if (OtaWire::isSha256Hex(now)) r["partitionSha256"] = now;
  }
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t st;
  const bool bootable = other && !(esp_ota_get_state_partition(other, &st) == ESP_OK && (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED));
  if (bootable && strcmp(status, "updating") != 0) {
    OtaWire::SlotRecord prev;
    char pnow[65] = "";
    if (const char* f = fileShaOf(other, prev, pnow)) {
      JsonObject p = d["previous"].to<JsonObject>();
      p["sha256"] = f;
      if (*prev.version) p["version"] = prev.version;
    }
  }
  d["values"]["bootCount"] = g_boots;
  send(d);
}

int sha256Start(mbedtls_sha256_context* c) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_starts(c, 0);
#else
  return mbedtls_sha256_starts_ret(c, 0);
#endif
}
int sha256Update(mbedtls_sha256_context* c, const uint8_t* b, size_t n) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_update(c, b, n);
#else
  return mbedtls_sha256_update_ret(c, b, n);
#endif
}
int sha256Finish(mbedtls_sha256_context* c, uint8_t out[32]) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_finish(c, out);
#else
  return mbedtls_sha256_finish_ret(c, out);
#endif
}

// Refused: what the device says, then what runs (unchanged).
void refuse(const String& why) {
  reply("refused: " + why);
  state("running");
}

void runUpdate(const Creds& c) {
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  if (!target) return refuse("no OTA slot to write");
  char url[320];
  if (!OtaWire::joinUrl(c.apiBaseUrl.c_str(), g_update.path, url, sizeof url)) return refuse("the API base is not http(s)://host[:port]");
  state("updating");

  WiFiClientSecure tls;
  WiFiClient plain;
  // As every call to the host: TLS without checking its certificate (README § Security); the device key authenticates.
  tls.setInsecure();
  const bool https = !strncmp(url, "https://", 8);
  HTTPClient http;
  if (!(https ? http.begin(tls, url) : http.begin(plain, url))) return refuse("the image url could not be opened");
  http.addHeader("Authorization", "Bearer " + c.apiKey);
  http.setTimeout(20000);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    return refuse("the image route answered " + String(code) + "; nothing was written");
  }
  const int announced = http.getSize();
  if (announced >= 0 && announced != g_update.size) {
    http.end();
    return refuse("the host sends " + String(announced) + " bytes; the update states " + String((long)g_update.size) + "; nothing was written");
  }
  // The slot is about to change: what was kept of it no longer holds.
  {
    Preferences p;
    if (p.begin(kNs, false)) {
      p.remove(key('f', target->label).c_str());
      p.remove(key('p', target->label).c_str());
      p.remove(key('v', target->label).c_str());
      p.end();
    }
  }
  esp_ota_handle_t h = 0;
  esp_err_t e = esp_ota_begin(target, (size_t)g_update.size, &h);
  if (e != ESP_OK) {
    http.end();
    return refuse(String("esp_ota_begin: ") + esp_err_to_name(e));
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  sha256Start(&sha);
  OtaWire::Receive rx;
  rx.expected = g_update.size;
  static uint8_t buf[4096];
  WiFiClient* stream = http.getStreamPtr();
  uint32_t lastByte = millis(), lastLoop = millis();
  String failed;
  while (rx.got < rx.expected) {
    const size_t avail = stream->available();
    if (avail) {
      const int n = stream->readBytes(buf, avail < sizeof buf ? avail : sizeof buf);
      if (n <= 0) continue;
      if (!rx.take((size_t)n)) break;
      sha256Update(&sha, buf, (size_t)n);
      if ((e = esp_ota_write(h, buf, (size_t)n)) != ESP_OK) {
        failed = String("esp_ota_write: ") + esp_err_to_name(e);
        break;
      }
      lastByte = millis();
    } else if (!stream->connected()) {
      break;
    } else if (millis() - lastByte > 20000) {
      failed = "no bytes for 20 s";
      break;
    } else {
      delay(2);
    }
    // Keep the socket served (pings); a second update meanwhile is refused as busy.
    if (millis() - lastLoop > 500) {
      CloudWs::loop();
      lastLoop = millis();
    }
  }
  delay(20);
  if (!failed.length() && stream->available() > 0) rx.over = true;
  http.end();
  uint8_t d[32];
  sha256Finish(&sha, d);
  mbedtls_sha256_free(&sha);
  char got[65];
  OtaWire::toHex(d, got);
  if (failed.length()) {
    esp_ota_abort(h);
    return refuse(failed + " after " + String((long)rx.got) + " bytes; nothing was booted");
  }
  switch (OtaWire::verdict(rx, g_update.sha, got)) {
    case OtaWire::Verdict::Size:
      esp_ota_abort(h);
      return refuse("came " + String((long)rx.got) + (rx.over ? "+" : "") + " bytes, the update states " + String((long)rx.expected) + "; nothing was booted");
    case OtaWire::Verdict::Digest:
      esp_ota_abort(h);
      return refuse(String("the image's SHA-256 is ") + got + ", not " + g_update.sha + "; nothing was booted");
    case OtaWire::Verdict::Ok:
      break;
  }
  if ((e = esp_ota_end(h)) != ESP_OK) return refuse(String("the image is not an app this chip boots (esp_ota_end: ") + esp_err_to_name(e) + "); nothing was booted");
  char part[65] = "";
  partitionSha(target, part);
  {
    Preferences p;
    if (p.begin(kNs, false)) {
      p.putString(key('f', target->label).c_str(), got);
      p.putString(key('p', target->label).c_str(), part);
      p.putString(key('v', target->label).c_str(), g_update.version);
      p.end();
    }
  }
  setPending(target->label);
  if ((e = esp_ota_set_boot_partition(target)) != ESP_OK) {
    setPending(nullptr);
    return refuse(String("esp_ota_set_boot_partition: ") + esp_err_to_name(e));
  }
  reply(String("written ") + got + " to " + target->label + "; booting it. It confirms itself when it reaches the host again");
  CloudWs::loop();
  delay(300);
  ESP.restart();
}

void runRollback() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  // A new image that has not confirmed goes back the bootloader's way.
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    reply(String("rolling back: ") + run->label + " has not confirmed; the bootloader boots the slot before it");
    CloudWs::loop();
    delay(300);
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return;
  }
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!other || (esp_ota_get_state_partition(other, &st) == ESP_OK && (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED)))
    return refuse("the other slot holds no image that ran: nothing to go back to");
  // Checks the image in the slot before it changes the boot slot.
  const esp_err_t e = esp_ota_set_boot_partition(other);
  if (e != ESP_OK) return refuse(String("the other slot holds no image this chip boots (") + esp_err_to_name(e) + ")");
  reply(String("rolling back: booting ") + other->label);
  CloudWs::loop();
  delay(300);
  ESP.restart();
}
}  // namespace

namespace Ota {

void begin() {
  Preferences p;
  if (p.begin(kNs, false)) {
    g_boots = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", g_boots);
    p.end();
  }
  const esp_partition_t* run = esp_ota_get_running_partition();
  const String pend = pending();
  switch (OtaWire::bootOutcome(pend.c_str(), run ? run->label : "")) {
    case OtaWire::Boot::Plain:
      break;
    case OtaWire::Boot::Confirming: {
      esp_ota_img_states_t st;
      if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        g_confirming = true;
        g_confirmBy = millis() + CAGI_OTA_CONFIRM_MS;
        Serial.printf("[ota] %s is new: it confirms when the socket reaches the host (within %u s)\n", run->label, (unsigned)(CAGI_OTA_CONFIRM_MS / 1000));
      } else {
        setPending(nullptr);
      }
      break;
    }
    case OtaWire::Boot::WentBack: {
      const OtaWire::SlotRecord r = recordOf(pend.c_str());
      g_bootReply = String("the image ") + (OtaWire::isSha256Hex(r.fileSha) ? r.fileSha : "(unknown)") + " in " + pend + " did not confirm; the bootloader booted " + (run ? run->label : "?") + " again";
      setPending(nullptr);
      break;
    }
  }
}

bool onMessage(const uint8_t* payload, size_t len) {
  if (len < 20 || memmem(payload, len, "\"firmware_", 10) == nullptr) return false;
  JsonDocument m;
  if (deserializeJson(m, payload, len)) return false;
  const String type = m["type"] | "";
  if (!type.startsWith("firmware_")) return false;
  if (type == "firmware_report") {
    g_report = true;
    return true;
  }
  if (type != "firmware_update" && type != "firmware_rollback") return true;
  if (g_busy || g_job != Job::None) {
    reply("busy: an update or a rollback is under way");
    return true;
  }
  if (type == "firmware_rollback") {
    g_job = Job::Rollback;
    return true;
  }
  const char* sha = m["sha256"] | "";
  const long long size = m["size"].is<long>() ? (long long)m["size"].as<long>() : -1;
  const char* path = m["url"] | "";
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  if (const char* why = OtaWire::checkUpdate(sha, size, path, target ? target->size : 0)) {
    refuse(why);
    return true;
  }
  strlcpy(g_update.sha, sha, sizeof g_update.sha);
  g_update.size = size;
  strlcpy(g_update.version, m["version"] | "", sizeof g_update.version);
  strlcpy(g_update.path, path, sizeof g_update.path);
  g_job = Job::Update;
  return true;
}

void onConnected() {
  if (!g_confirming) return;
  const esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
  g_confirming = false;
  setPending(nullptr);
  const esp_partition_t* run = esp_ota_get_running_partition();
  g_bootReply = e == ESP_OK ? String("the image in ") + (run ? run->label : "?") + " reached the host and is confirmed"
                            : String("the image could not be confirmed: ") + esp_err_to_name(e);
}

void loop(const Creds& c) {
  if (!CloudWs::connected()) return;
  if (g_report) {
    g_report = false;
    if (g_bootReply.length()) {
      reply(g_bootReply);
      g_bootReply = "";
    }
    state("running");
  }
  const Job job = g_job;
  if (job == Job::None) return;
  g_busy = true;
  g_job = Job::None;
  if (job == Job::Update) runUpdate(c);
  else runRollback();
  g_busy = false;
}

void watch() {
  if (g_confirming && (int32_t)(millis() - g_confirmBy) >= 0) {
    Serial.println("[ota] the new image did not reach the host in time: back to the slot before it");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

}  // namespace Ota
