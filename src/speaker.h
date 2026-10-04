#pragma once
#include <Arduino.h>

// Speaker output (I2S amplifier). Built only with -DCAGI_SPEAKER_ENABLED=1; otherwise every call is a
// no-op stub. The realtime socket (cloud_ws.cpp) turns `play_audio` / `present_stop` / `config` into
// these calls and sends each Result back as an `action_result`. README.md § Speaker has the contract.
//
// One clip at a time. A speaker task fetches, decodes and plays it, so the frame loop never waits for
// the network, the MP3 decoder or the amplifier. The device never retries a clip.
namespace Speaker {

// Install the I2S TX driver, hold the amplifier in shutdown, start the speaker task.
bool begin();
bool available();

// The API origin and key. The task sends the key as a Bearer token only to a clip URL on that host,
// never to another host.
void setAuth(const String& apiBaseUrl, const String& apiKey);

struct Request {
  String requestId;          // echoed in the Result; may be empty
  String url;                // https URL of the clip, or empty when `bytes` holds it
  uint8_t* bytes = nullptr;  // the decoded base64 clip (heap); ownership passes to play()
  size_t len = 0;
  String hint;               // `format` or `mime` from the command; may be empty
  bool interrupt = true;     // false: refuse while another clip is loading or playing
};

// Queue a clip. Returns nullptr when accepted (a Result follows later), or the reason it is refused
// now (no Result follows). Always takes ownership of r.bytes.
const char* play(Request& r);
// present_stop: stop the clip that plays now and drop one that waits. Each of those that has not
// reported yet reports { ok:false, error:"stopped" }.
void stop();
// The operator's output switch (`config.outputs.speaker`). false stops playback and refuses play().
void setEnabled(bool on);
bool enabled();

// Milliseconds since the speaker last made sound: 0 while a clip plays, UINT32_MAX if it never played.
// The main loop does not post mic clips recorded while the speaker played.
uint32_t quietForMs();

struct Result {
  String requestId;
  bool ok = false;
  uint32_t durationMs = 0;  // ok: the decoded clip length
  String error;             // !ok: why
};
// Take the next Result (the main loop sends it over the socket). false when there is none.
bool pollResult(Result* out);

}  // namespace Speaker
