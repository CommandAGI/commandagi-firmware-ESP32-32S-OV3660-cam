#include "chip_lock.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_flash_encrypt.h>
#include <esp_partition.h>
#include <esp_secure_boot.h>
#include <string.h>
#include "config.h"

namespace {
#if CAGI_PROV_PIN_NVS
// Namespace `cagi-fac`: what the factory wrote. A factory-reset erases only `cagi`.
char g_pin[7] = "";
bool g_pinLoaded = false;
#endif

bool sixDigits(const char* s) {
  for (int i = 0; i < 6; i++)
    if (s[i] < '0' || s[i] > '9') return false;
  return s[6] == 0;
}
}  // namespace

namespace ChipLock {

ChipLockStatus::Facts facts() {
  using ChipLockStatus::Download;
  using ChipLockStatus::Flash;
  ChipLockStatus::Facts f{};
  f.secureBoot = esp_secure_boot_enabled();
  f.flash = !esp_flash_encryption_enabled() ? Flash::Plain
            : esp_get_flash_encryption_mode() == ESP_FLASH_ENC_MODE_RELEASE ? Flash::Release
                                                                             : Flash::Development;
#if CONFIG_NVS_ENCRYPTION
  // nvs_flash_init() with CONFIG_NVS_ENCRYPTION opens NVS only with the keys in nvs_keys, so NVS is
  // encrypted whenever it works. Those keys are protected only when flash encryption covers their partition.
  const esp_partition_t* keys = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, nullptr);
  f.nvsEncrypted = keys && keys->encrypted && f.flash != Flash::Plain;
#else
  f.nvsEncrypted = false;
#endif
#if CONFIG_IDF_TARGET_ESP32S3
  f.jtagOff = esp_efuse_read_field_bit(ESP_EFUSE_HARD_DIS_JTAG) && esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_JTAG);
  f.download = esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE)          ? Download::Off
               : esp_efuse_read_field_bit(ESP_EFUSE_ENABLE_SECURITY_DOWNLOAD) ? Download::Secure
                                                                              : Download::Open;
#elif CONFIG_IDF_TARGET_ESP32
  f.jtagOff = esp_efuse_read_field_bit(ESP_EFUSE_DISABLE_JTAG);
  f.download = esp_efuse_read_field_bit(ESP_EFUSE_UART_DOWNLOAD_DIS) ? Download::Off : Download::Open;
#else
  f.jtagOff = false;
  f.download = Download::Open;
#endif
  return f;
}

const char* provPin() {
#if CAGI_PROV_PIN_NVS
  if (!g_pinLoaded) {
    Preferences p;
    if (p.begin("cagi-fac", true)) {
      const String s = p.getString("pin", "");
      p.end();
      if (sixDigits(s.c_str())) strcpy(g_pin, s.c_str());
    }
    g_pinLoaded = true;
  }
  return g_pin[0] ? g_pin : nullptr;
#else
  return CAGI_PROV_PIN;
#endif
}

bool command(const char* line) {
  if (strncmp(line, "prov-pin ", 9)) return false;
#if CAGI_PROV_PIN_NVS
  const char* pin = line + 9;
  if (!sixDigits(pin)) {
    Serial.println("prov-pin refused: the PIN is six digits");
    return true;
  }
  // Once: a person with the unit and a cable must not replace the PIN on its label.
  if (provPin()) {
    Serial.println("prov-pin refused: this unit has its PIN (the factory sets it once)");
    return true;
  }
  Preferences p;
  if (!p.begin("cagi-fac", false) || p.putString("pin", pin) != 6) {
    p.end();
    Serial.println("prov-pin refused: NVS write failed");
    return true;
  }
  p.end();
  strcpy(g_pin, pin);
  Serial.println("prov-pin ok");
#else
  Serial.println("prov-pin refused: this build compiles its PIN in (CAGI_PROV_PIN)");
#endif
  return true;
}

}  // namespace ChipLock
