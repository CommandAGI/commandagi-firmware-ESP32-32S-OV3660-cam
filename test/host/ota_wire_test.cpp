// Host test of src/ota_wire.h: g++ -std=c++17 -O1 -Wall -Isrc test/host/ota_wire_test.cpp -o /tmp/ota_wire_test && /tmp/ota_wire_test
// What an update must say, where the image comes from, whether the bytes are the image, and what a slot's kept digest is worth.
#include <cstdio>
#include <cstring>
#include <string>
#include "ota_wire.h"

static int failures = 0;
#define EXPECT(c)                                              \
  do {                                                         \
    if (!(c)) {                                                \
      fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
      failures++;                                              \
    }                                                          \
  } while (0)

int main() {
  const std::string sha(64, 'a'), other = std::string(63, 'a') + "b";
  const std::string path = "/v1/firmware/s-0011223344556677/d-8899aabbccddeeff/" + sha;
  const size_t slot = 0x1E0000;

  EXPECT(OtaWire::isSha256Hex(sha.c_str()));
  EXPECT(!OtaWire::isSha256Hex(std::string(64, 'A').c_str()));
  EXPECT(!OtaWire::isSha256Hex(std::string(63, 'a').c_str()));
  EXPECT(!OtaWire::isSha256Hex(nullptr));

  // An update: a digest, a size that fits the slot, and the host's path that ends with that digest.
  EXPECT(OtaWire::checkUpdate(sha.c_str(), 1000, path.c_str(), slot) == nullptr);
  EXPECT(OtaWire::checkUpdate("ab", 1000, path.c_str(), slot) != nullptr);
  EXPECT(OtaWire::checkUpdate(sha.c_str(), 0, path.c_str(), slot) != nullptr);
  EXPECT(OtaWire::checkUpdate(sha.c_str(), (long long)slot + 1, path.c_str(), slot) != nullptr);
  EXPECT(OtaWire::checkUpdate(other.c_str(), 1000, path.c_str(), slot) != nullptr);  // the path names another digest
  EXPECT(OtaWire::checkUpdate(sha.c_str(), 1000, ("https://evil.example" + path).c_str(), slot) != nullptr);
  EXPECT(OtaWire::checkUpdate(sha.c_str(), 1000, ("/v1/firmware/../x/" + sha).c_str(), slot) != nullptr);
  EXPECT(OtaWire::checkUpdate(sha.c_str(), 1000, (path + "?x=1").c_str(), slot) != nullptr);

  // The image comes from the origin the device was provisioned with, never from another host.
  char url[320];
  EXPECT(OtaWire::joinUrl("https://192.168.1.20:8787", path.c_str(), url, sizeof url) && std::string(url) == "https://192.168.1.20:8787" + path);
  EXPECT(OtaWire::joinUrl("http://host.local:4174/api/", path.c_str(), url, sizeof url) && std::string(url) == "http://host.local:4174" + path);
  EXPECT(!OtaWire::joinUrl("ftp://x", path.c_str(), url, sizeof url));
  EXPECT(!OtaWire::joinUrl("https://", path.c_str(), url, sizeof url));
  EXPECT(!OtaWire::joinUrl("https://h", path.c_str(), url, 20));

  // The bytes: exactly the stated size, then the digest.
  OtaWire::Receive r;
  r.expected = 10;
  EXPECT(r.take(4) && r.take(6));
  EXPECT(OtaWire::verdict(r, sha.c_str(), sha.c_str()) == OtaWire::Verdict::Ok);
  EXPECT(OtaWire::verdict(r, sha.c_str(), other.c_str()) == OtaWire::Verdict::Digest);
  EXPECT(!r.take(1) && OtaWire::verdict(r, sha.c_str(), sha.c_str()) == OtaWire::Verdict::Size);
  OtaWire::Receive shortRx;
  shortRx.expected = 10;
  shortRx.take(9);
  EXPECT(OtaWire::verdict(shortRx, sha.c_str(), sha.c_str()) == OtaWire::Verdict::Size);

  uint8_t d[32];
  for (int i = 0; i < 32; i++) d[i] = (uint8_t)(i * 9);
  char hex[65];
  OtaWire::toHex(d, hex);
  EXPECT(std::string(hex) == "0009121b242d363f48515a636c757e879099a2abb4bdc6cfd8e1eaf3fc050e17");

  // A slot's kept file digest holds only while the slot is as it was written.
  OtaWire::SlotRecord rec;
  strcpy(rec.fileSha, sha.c_str());
  strcpy(rec.partitionSha, other.c_str());
  EXPECT(OtaWire::fileShaOf(rec, other.c_str()) != nullptr && std::string(OtaWire::fileShaOf(rec, other.c_str())) == sha);
  EXPECT(OtaWire::fileShaOf(rec, std::string(64, 'c').c_str()) == nullptr);  // flashed again over USB
  EXPECT(OtaWire::fileShaOf(OtaWire::SlotRecord{}, other.c_str()) == nullptr);  // never written by an update
  EXPECT(OtaWire::fileShaOf(rec, nullptr) == nullptr);

  // After a boot: the new slot runs (it confirms when it reaches the host), or the bootloader went back.
  EXPECT(OtaWire::bootOutcome("", "app0") == OtaWire::Boot::Plain);
  EXPECT(OtaWire::bootOutcome("app1", "app1") == OtaWire::Boot::Confirming);
  EXPECT(OtaWire::bootOutcome("app1", "app0") == OtaWire::Boot::WentBack);

  if (failures) return 1;
  puts("ota_wire: ok");
  return 0;
}
