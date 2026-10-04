#include "cellular.h"
#include "config.h"

#if CAGI_CELLULAR_ENABLED
#include <driver/uart.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_netif_ppp.h>

namespace {
constexpr uart_port_t kUart = (uart_port_t)CAGI_MODEM_UART;
constexpr int kRxBuf = 16 * 1024;  // no RTS/CTS on either carrier: a large RX buffer instead

SemaphoreHandle_t g_lock = nullptr;  // guards g_info
Cellular::Info g_info;
volatile bool g_want = false;
volatile bool g_up = false;
volatile bool g_pppDead = false;
String g_apn, g_simPin;
bool g_pinTried = false;     // the SIM PIN goes out at most once per boot
bool g_fatal = false;        // SIM PIN rejected or SIM locked: stop until a reboot or re-provisioning
uint32_t g_signalAt = 0;
uint32_t g_offAt = 0;        // when the modem was last switched off (PWRKEY needs >= 2 s off)
esp_netif_t* g_ppp = nullptr;

struct PppDriver {
  esp_netif_driver_base_t base;
} g_drv;

void setState(const char* st, const char* err = nullptr) {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  const bool changed = g_info.state != st || (err && g_info.error != err);
  g_info.state = st;
  g_info.error = err ? err : "";
  xSemaphoreGive(g_lock);
  if (changed) Serial.printf("[cell] %s%s%s\n", st, err ? ": " : "", err ? err : "");
}

void drive(int pin, bool high) {
  if (pin >= 0) digitalWrite(pin, high ? HIGH : LOW);
}

bool statusHigh() { return CAGI_MODEM_STATUS_PIN >= 0 && digitalRead(CAGI_MODEM_STATUS_PIN) == HIGH; }

// ── AT ────────────────────────────────────────────────────────────────────────────────────────
// Read one line (without CR/LF). Returns its length, or 0 when nothing came before the deadline.
size_t readLine(char* buf, size_t cap, uint32_t deadline) {
  size_t n = 0;
  while ((int32_t)(deadline - millis()) > 0) {
    uint8_t c;
    if (uart_read_bytes(kUart, &c, 1, pdMS_TO_TICKS(20)) != 1) continue;
    if (c == '\r') continue;
    if (c == '\n') {
      if (n == 0) continue;
      break;
    }
    if (n + 1 < cap) buf[n++] = (char)c;
  }
  buf[n] = 0;
  return n;
}

// Send one AT command and wait for `until` at the start of a line (default OK). Lines other than the
// echo and the final result go to *out. Commands are not logged: AT+CPIN carries the SIM PIN.
bool at(const char* cmd, String* out, uint32_t timeoutMs, const char* until = "OK") {
  uart_flush_input(kUart);
  uart_write_bytes(kUart, cmd, strlen(cmd));
  uart_write_bytes(kUart, "\r", 1);
  const uint32_t deadline = millis() + timeoutMs;
  char line[160];
  while ((int32_t)(deadline - millis()) > 0) {
    if (!readLine(line, sizeof line, deadline)) continue;
    if (strcmp(line, cmd) == 0) continue;  // echo (before ATE0)
    if (strncmp(line, until, strlen(until)) == 0) return true;
    if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR", 10) == 0 || strcmp(line, "NO CARRIER") == 0 ||
        strcmp(line, "BUSY") == 0 || strcmp(line, "NO DIALTONE") == 0) {
      if (out) *out += line;
      return false;
    }
    if (out) {
      *out += line;
      *out += '\n';
    }
  }
  return false;
}

bool sync(uint32_t ms) {
  const uint32_t deadline = millis() + ms;
  while ((int32_t)(deadline - millis()) > 0) {
    if (at("AT", nullptr, 500)) return true;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return false;
}

// A modem that was online when the ESP32 reset may still be in PPP data mode: leave it with the
// escape sequence (1 s guard time on each side) and hang up.
void escapeDataMode() {
  vTaskDelay(pdMS_TO_TICKS(1100));
  uart_write_bytes(kUart, "+++", 3);
  vTaskDelay(pdMS_TO_TICKS(1100));
  at("ATH", nullptr, 3000);
}

// Find the modem's baud rate: its power-on rate, or the fast rate an earlier boot set with AT+IPR.
bool syncAnyBaud(uint32_t ms) {
  uart_set_baudrate(kUart, CAGI_MODEM_BAUD);
  if (sync(ms)) return true;
  if (CAGI_MODEM_BAUD_FAST == CAGI_MODEM_BAUD) return false;
  uart_set_baudrate(kUart, CAGI_MODEM_BAUD_FAST);
  return sync(2000);
}

void pulsePwrkey(uint32_t ms) {
  drive(CAGI_MODEM_PWRKEY_PIN, true);
  vTaskDelay(pdMS_TO_TICKS(ms));
  drive(CAGI_MODEM_PWRKEY_PIN, false);
}

bool powerOn() {
  setState("powering");
  if (statusHigh()) {
    // On already (an ESP32 reset does not reset the modem). Hang up whatever it was doing.
    if (syncAnyBaud(2000)) return true;
    escapeDataMode();
    if (syncAnyBaud(3000)) return true;
  } else {
    const uint32_t since = millis() - g_offAt;
    if (g_offAt && since < 2000) vTaskDelay(pdMS_TO_TICKS(2000 - since));
    pulsePwrkey(100);  // datasheet Ton 50 ms typ
    if (CAGI_MODEM_STATUS_PIN >= 0) {
      const uint32_t t0 = millis();
      while (!statusHigh() && millis() - t0 < 15000) vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  return syncAnyBaud(15000);
}

// Recovery for a modem that does not answer: RESET, or the supply rail, or a long PWRKEY press.
void hardRecover() {
  Serial.println("[cell] modem does not answer AT: hard recovery");
  if (CAGI_MODEM_RESET_PIN >= 0) {
    drive(CAGI_MODEM_RESET_PIN, true);
    vTaskDelay(pdMS_TO_TICKS(2600));
    drive(CAGI_MODEM_RESET_PIN, false);
  } else if (CAGI_MODEM_RAIL_OFF_PIN >= 0) {
    drive(CAGI_MODEM_RAIL_OFF_PIN, true);
    vTaskDelay(pdMS_TO_TICKS(3000));
    drive(CAGI_MODEM_RAIL_OFF_PIN, false);  // the modem stays off until the next PWRKEY pulse
  } else {
    pulsePwrkey(2600);  // a long press switches it off
  }
  g_offAt = millis();
}

void fastBaud() {
  if (CAGI_MODEM_BAUD_FAST == CAGI_MODEM_BAUD) return;
  char cmd[24];
  snprintf(cmd, sizeof cmd, "AT+IPR=%d", CAGI_MODEM_BAUD_FAST);
  if (!at(cmd, nullptr, 1000)) return;
  vTaskDelay(pdMS_TO_TICKS(100));
  uart_set_baudrate(kUart, CAGI_MODEM_BAUD_FAST);
  if (sync(2000)) return;
  uart_set_baudrate(kUart, CAGI_MODEM_BAUD);  // the fast rate does not work on this link: go back
  sync(2000);
}

// SIM: READY, or one try of the provisioned PIN. Returns false (and sets the state) otherwise.
bool sim() {
  setState("sim");
  String r;
  for (int i = 0; i < 5 && !at("AT+CPIN?", &r, 5000); i++) {  // the SIM can take a few seconds after power-on
    r = "";
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  if (r.indexOf("READY") >= 0) return true;
  if (r.indexOf("SIM PUK") >= 0) {
    g_fatal = true;
    setState("error", "SIM locked (PUK needed)");
    return false;
  }
  if (r.indexOf("SIM PIN") >= 0) {
    if (!g_simPin.length()) {
      g_fatal = true;
      setState("error", "SIM needs a PIN; provision simPin");
      return false;
    }
    if (g_pinTried) {
      g_fatal = true;
      setState("error", "SIM PIN rejected; not retried");
      return false;
    }
    g_pinTried = true;
    String cmd = "AT+CPIN=\"" + g_simPin + "\"";
    at(cmd.c_str(), nullptr, 5000);
    vTaskDelay(pdMS_TO_TICKS(3000));
    r = "";
    at("AT+CPIN?", &r, 5000);
    if (r.indexOf("READY") >= 0) return true;
    g_fatal = true;
    setState("error", "SIM PIN rejected; not retried");
    return false;
  }
  setState("error", "no SIM");
  return false;
}

// Parse the integer after the n-th comma of the first line starting with `prefix`.
bool field(const String& r, const char* prefix, int n, String* out) {
  int pos = r.indexOf(prefix);
  if (pos < 0) return false;
  pos += strlen(prefix);
  int end = r.indexOf('\n', pos);
  String line = r.substring(pos, end < 0 ? r.length() : end);
  for (int i = 0; i < n; i++) {
    int c = line.indexOf(',');
    if (c < 0) return false;
    line = line.substring(c + 1);
  }
  int c = line.indexOf(',');
  *out = (c < 0 ? line : line.substring(0, c));
  out->trim();
  return out->length() > 0;
}

void readSignal() {
  String r, v;
  int rssi = 0, rsrp10 = 0;
  if (at("AT+CSQ", &r, 2000) && field(r, "+CSQ:", 0, &v)) {
    const int n = v.toInt();
    if (n >= 0 && n <= 31) rssi = -113 + 2 * n;  // 27.007 §8.5
  }
  // A7670 / SIM7600: "+CPSI: LTE,Online,MCC-MNC,TAC,SCellID,PCellID,Band,EARFCN,DL,UL,RSRQ,RSRP,RSSI,RSSNR".
  // RSRP is in tenths of a dBm. Not verified on an A7670 here.
  r = "";
  if (at("AT+CPSI?", &r, 2000) && r.indexOf("LTE") >= 0 && field(r, "+CPSI:", 11, &v)) rsrp10 = v.toInt();
  r = "";
  at("AT+COPS=3,0", nullptr, 2000);  // long alphanumeric operator name
  String op;
  if (at("AT+COPS?", &r, 5000)) {
    int q1 = r.indexOf('"'), q2 = q1 >= 0 ? r.indexOf('"', q1 + 1) : -1;
    if (q2 > q1) op = r.substring(q1 + 1, q2);
  }
  xSemaphoreTake(g_lock, portMAX_DELAY);
  g_info.rssiDbm = rssi;
  g_info.rsrpDbm10 = rsrp10;
  g_info.op = op;
  xSemaphoreGive(g_lock);
  g_signalAt = millis();
}

bool registerNetwork() {
  setState("registering");
  at("AT+CNMP=38", nullptr, 3000);  // LTE only: a GSM burst draws more than the carrier's supply gives
  if (g_apn.length()) {
    String cmd = "AT+CGDCONT=1,\"IP\",\"" + g_apn + "\"";
    at(cmd.c_str(), nullptr, 3000);
  }
  const uint32_t t0 = millis();
  while (g_want && millis() - t0 < 180000) {
    String r, v;
    if (at("AT+CEREG?", &r, 2000) && field(r, "+CEREG:", 1, &v)) {
      const int stat = v.toInt();
      if (stat == 1 || stat == 5) return true;  // home / roaming
      if (stat == 3) {
        setState("error", "registration denied");
        return false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  if (g_want) setState("error", "not registered after 180 s");
  return false;
}

// ── PPP ───────────────────────────────────────────────────────────────────────────────────────
esp_err_t pppTransmit(void*, void* buffer, size_t len) {
  uart_write_bytes(kUart, (const char*)buffer, len);
  return ESP_OK;
}

esp_err_t pppPostAttach(esp_netif_t* netif, void* args) {
  PppDriver* d = (PppDriver*)args;
  d->base.netif = netif;
  esp_netif_driver_ifconfig_t ifc = {};
  ifc.handle = d;
  ifc.transmit = pppTransmit;
  return esp_netif_set_driver_config(netif, &ifc);
}

void onIpEvent(void*, esp_event_base_t, int32_t id, void* data) {
  if (id == IP_EVENT_PPP_GOT_IP) {
    const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
    char ip[16];
    esp_ip4addr_ntoa(&e->ip_info.ip, ip, sizeof ip);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_info.ip = ip;
    xSemaphoreGive(g_lock);
    g_up = true;
  } else if (id == IP_EVENT_PPP_LOST_IP) {
    g_up = false;
    g_pppDead = true;
  }
}

void onPppStatus(void*, esp_event_base_t, int32_t id, void*) {
  if (id > NETIF_PPP_ERRORNONE && id < NETIF_PP_PHASE_OFFSET) g_pppDead = true;  // an error ends the session
}

bool setupNetif() {
  esp_netif_init();
  esp_err_t e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
  esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
  g_ppp = esp_netif_new(&cfg);
  if (!g_ppp) return false;
  g_drv.base.post_attach = pppPostAttach;
  if (esp_netif_attach(g_ppp, &g_drv) != ESP_OK) return false;
  esp_netif_ppp_config_t pcfg = {true, true};
  esp_netif_ppp_set_params(g_ppp, &pcfg);
  esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, onIpEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, onIpEvent, nullptr);
  esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, onPppStatus, nullptr);
  return true;
}

// Dial, run PPP until the policy hangs up or the session dies, then return to command mode.
void runPpp() {
  setState("dialing");
  if (!at("ATD*99#", nullptr, 15000, "CONNECT")) {
    setState("error", "dial failed");
    return;
  }
  g_pppDead = false;
  esp_netif_action_start(g_ppp, 0, 0, nullptr);
  setState("online");
  static uint8_t buf[1024];
  bool stopping = false;
  uint32_t stopAt = 0;
  for (;;) {
    const int n = uart_read_bytes(kUart, buf, sizeof buf, pdMS_TO_TICKS(10));
    if (n > 0) esp_netif_receive(g_ppp, buf, n, nullptr);
    if (!stopping && (!g_want || g_pppDead)) {
      stopping = true;
      stopAt = millis();
      esp_netif_action_stop(g_ppp, 0, 0, nullptr);  // LCP terminate; keep pumping so it can finish
    }
    if (stopping && millis() - stopAt > 2000) break;
  }
  g_up = false;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  g_info.ip = "";
  xSemaphoreGive(g_lock);
  escapeDataMode();
  setState(g_want ? "registering" : "idle");
}

void modemTask(void*) {
  if (!setupNetif()) {
    setState("error", "PPP netif setup failed");
    vTaskDelete(nullptr);
    return;
  }
  bool powered = false;
  uint32_t backoffMs = 5000;
  for (;;) {
    if (!g_want || g_fatal) {
      if (!g_fatal) setState(powered ? "idle" : "off");
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!powered) {
      if (!powerOn()) {
        hardRecover();
        setState("error", "modem does not answer AT");
        vTaskDelay(pdMS_TO_TICKS(backoffMs));
        backoffMs = backoffMs < 120000 ? backoffMs * 2 : 120000;
        continue;
      }
      at("ATE0", nullptr, 1000);
      at("AT+CMEE=2", nullptr, 1000);
      fastBaud();
      powered = true;
    }
    if (!sim() || !registerNetwork()) {
      if (!g_fatal) vTaskDelay(pdMS_TO_TICKS(backoffMs));
      backoffMs = backoffMs < 120000 ? backoffMs * 2 : 120000;
      if (!sync(2000)) powered = false;  // the modem stopped answering: power it on again
      continue;
    }
    readSignal();
    runPpp();
    backoffMs = 5000;
    if (!sync(3000)) powered = false;
  }
}
}  // namespace

namespace Cellular {

void begin(const String& apn, const String& simPin) {
  if (g_lock) return;
  g_lock = xSemaphoreCreateMutex();
  g_apn = apn;
  g_simPin = simPin;
  g_info.state = "off";
  if (CAGI_MODEM_PWRKEY_PIN >= 0) { pinMode(CAGI_MODEM_PWRKEY_PIN, OUTPUT); drive(CAGI_MODEM_PWRKEY_PIN, false); }
  if (CAGI_MODEM_RESET_PIN >= 0) { pinMode(CAGI_MODEM_RESET_PIN, OUTPUT); drive(CAGI_MODEM_RESET_PIN, false); }
  if (CAGI_MODEM_RAIL_OFF_PIN >= 0) { pinMode(CAGI_MODEM_RAIL_OFF_PIN, OUTPUT); drive(CAGI_MODEM_RAIL_OFF_PIN, false); }
  if (CAGI_MODEM_STATUS_PIN >= 0) pinMode(CAGI_MODEM_STATUS_PIN, INPUT_PULLDOWN);

  uart_config_t uc = {};
  uc.baud_rate = CAGI_MODEM_BAUD;
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_APB;
  if (uart_driver_install(kUart, kRxBuf, 4096, 0, nullptr, 0) != ESP_OK || uart_param_config(kUart, &uc) != ESP_OK ||
      uart_set_pin(kUart, CAGI_MODEM_TX_PIN, CAGI_MODEM_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    g_info.state = "error";
    g_info.error = "modem UART setup failed";
    Serial.println("[cell] modem UART setup failed");
    return;
  }
  gpio_pullup_en((gpio_num_t)CAGI_MODEM_RX_PIN);  // the level shifter floats while the modem is off
  // Core 0 with Wi-Fi and lwIP: the task moves network bytes, like the Wi-Fi driver.
  xTaskCreatePinnedToCore(modemTask, "modem", 6144, nullptr, 3, nullptr, 0);
}

void want(bool on) {
  if (g_want != on) Serial.printf("[cell] link policy: %s\n", on ? "bring cellular up" : "hang up cellular");
  g_want = on;
}

bool up() { return g_up; }

Info info() {
  Info i;
  if (!g_lock) return i;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  i = g_info;
  xSemaphoreGive(g_lock);
  i.signalAgeMs = g_signalAt ? millis() - g_signalAt : 0;
  return i;
}

}  // namespace Cellular

#else
namespace Cellular {
void begin(const String&, const String&) {}
void want(bool) {}
bool up() { return false; }
Info info() { return Info(); }
}  // namespace Cellular
#endif
