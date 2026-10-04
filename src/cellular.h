#pragma once
#include <Arduino.h>

// LTE modem (SIMCom A7670-class) as a PPP network interface. Built only with -DCAGI_CELLULAR_ENABLED=1;
// otherwise every call is a stub. One task owns the modem's UART: it powers the modem on, runs the AT
// setup, dials, and then pumps PPP bytes between the UART and lwIP. HTTPS and the realtime socket use
// the PPP interface through lwIP's routing; nothing above this module knows which link carries them.
namespace Cellular {

// Start the modem task. The modem stays off until want(true). apn may be empty (the SIM's default);
// simPin may be empty. The SIM PIN is sent at most once per boot: a wrong PIN is not retried, because
// three wrong PINs lock the SIM.
void begin(const String& apn, const String& simPin);
// The link policy's request: true = bring PPP up (or keep it up), false = hang up. The modem stays
// powered and registered after a hang-up, so a later want(true) is fast.
void want(bool up);
// PPP has an IP address.
bool up();

struct Info {
  String state;        // off | powering | sim | registering | dialing | online | error
  String error;        // why, when state == error
  String op;           // operator name (AT+COPS), measured before the dial
  int rssiDbm = 0;     // from AT+CSQ; 0 = unknown
  int rsrpDbm10 = 0;   // RSRP x10 from AT+CPSI (LTE); 0 = unknown
  uint32_t signalAgeMs = 0;  // how old rssi/rsrp/op are: AT is not available while PPP runs
  String ip;
};
Info info();

}  // namespace Cellular
