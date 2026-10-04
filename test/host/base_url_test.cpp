// Host test of src/base_url.h: g++ -std=c++17 -O1 -Wall -Isrc test/host/base_url_test.cpp -o /tmp/base_url_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "base_url.h"

static int failures = 0;
static void expect(const char* base, bool ok, const char* host = "", unsigned port = 0, bool tls = false) {
  BaseUrl::Target t{};
  const bool got = BaseUrl::parse(base, t);
  if (got != ok || (ok && (strcmp(t.host, host) || t.port != port || t.tls != tls))) {
    fprintf(stderr, "FAIL %s: got %d %s:%u tls=%d\n", base, got, t.host, t.port, t.tls);
    failures++;
  }
}

int main() {
  expect("https://api.commandagi.com", true, "api.commandagi.com", 443, true);
  expect("https://api.commandagi.com/", true, "api.commandagi.com", 443, true);
  expect("https://192.168.1.20:4174", true, "192.168.1.20", 4174, true);
  expect("https://host.local:8443/api", true, "host.local", 8443, true);
  expect("http://192.168.1.20:4174", true, "192.168.1.20", 4174, false);
  expect("http://camera-host", true, "camera-host", 80, false);
  expect("ftp://x", false);
  expect("https://", false);
  expect("https://h:0", false);
  expect("https://h:70000", false);
  expect("https://h:12ab", false);
  expect("https://h:", false);
  if (failures) return 1;
  puts("base_url_test: ok");
  return 0;
}
