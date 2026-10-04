#pragma once
// Where the realtime socket goes, from the API base the owner provisioned (plain C++, host-tested): the scheme picks
// TLS, and a port in the base is kept, so the same firmware reaches the platform (https, 443) and a local host on the
// LAN (its own port, docs/devices.md in CommandAGI § sealed streams on the host).
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace BaseUrl {

struct Target {
  char host[128];
  unsigned port;
  bool tls;
};

// False when the base is not http(s)://host[:port][/path], or the host or port does not fit.
inline bool parse(const char* base, Target& out) {
  const char* p = base;
  if (!strncmp(p, "https://", 8)) {
    out.tls = true;
    p += 8;
  } else if (!strncmp(p, "http://", 7)) {
    out.tls = false;
    p += 7;
  } else {
    return false;
  }
  const size_t hostLen = strcspn(p, ":/?#");
  if (!hostLen || hostLen >= sizeof out.host) return false;
  memcpy(out.host, p, hostLen);
  out.host[hostLen] = 0;
  out.port = out.tls ? 443 : 80;
  if (p[hostLen] == ':') {
    char* end = nullptr;
    const unsigned long port = strtoul(p + hostLen + 1, &end, 10);
    if (end == p + hostLen + 1 || port == 0 || port > 65535 || (*end && !strchr("/?#", *end))) return false;
    out.port = (unsigned)port;
  }
  return true;
}

}  // namespace BaseUrl
