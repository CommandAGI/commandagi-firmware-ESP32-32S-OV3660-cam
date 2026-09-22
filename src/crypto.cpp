#include "crypto.h"
#include <esp_random.h>
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "config.h"

namespace {
constexpr size_t IV_LEN = 12;
constexpr size_t TAG_LEN = 16;
constexpr size_t KEY_LEN = 32;  // == SHA-256 output, so HKDF-Expand needs a single block

// HKDF-SHA256 (RFC 5869) for a 32-byte OKM, built from the one-shot HMAC (the prebuilt Arduino
// mbedtls doesn't export mbedtls_hkdf). PRK = HMAC(salt, IKM); OKM = HMAC(PRK, info || 0x01).
bool hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const char* info,
                uint8_t out[KEY_LEN]) {
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;
  uint8_t prk[KEY_LEN];
  if (mbedtls_md_hmac(md, salt, saltLen, ikm, ikmLen, prk) != 0) return false;

  const size_t infoLen = strlen(info);
  uint8_t t1in[80];
  if (infoLen + 1 > sizeof(t1in)) return false;
  memcpy(t1in, info, infoLen);
  t1in[infoLen] = 0x01;
  int rc = mbedtls_md_hmac(md, prk, KEY_LEN, t1in, infoLen + 1, out);
  memset(prk, 0, sizeof(prk));
  return rc == 0;
}
}  // namespace

namespace Crypto {

void randomBytes(uint8_t* out, size_t len) {
  esp_fill_random(out, len);
}

String toHex(const uint8_t* buf, size_t len) {
  static const char* hex = "0123456789abcdef";
  String s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    s += hex[buf[i] >> 4];
    s += hex[buf[i] & 0x0f];
  }
  return s;
}

bool openSealed(const uint8_t* wire, size_t wireLen, const char* pin, const uint8_t* salt, size_t saltLen,
                String& outJson) {
  if (wireLen <= IV_LEN + TAG_LEN) return false;  // need IV + at least 1 byte ct + tag

  // 1) Derive the key: HKDF-SHA256(PIN, salt, info).
  uint8_t key[KEY_LEN];
  if (!hkdfSha256(salt, saltLen, (const uint8_t*)pin, strlen(pin), CAGI_PROV_HKDF_INFO, key)) {
    return false;
  }

  // 2) Split the wire: IV(12) || ciphertext || tag(16).
  const uint8_t* iv = wire;
  const size_t ctLen = wireLen - IV_LEN - TAG_LEN;
  const uint8_t* ct = wire + IV_LEN;
  const uint8_t* tag = wire + IV_LEN + ctLen;

  // 3) AES-256-GCM authenticated decrypt.
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = false;
  uint8_t* pt = (uint8_t*)malloc(ctLen + 1);
  if (pt && mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_LEN * 8) == 0) {
    int rc = mbedtls_gcm_auth_decrypt(&gcm, ctLen, iv, IV_LEN, nullptr, 0, tag, TAG_LEN, ct, pt);
    if (rc == 0) {
      pt[ctLen] = 0;
      outJson = String((const char*)pt);
      ok = true;
    }
  }
  if (pt) {
    memset(pt, 0, ctLen + 1);
    free(pt);
  }
  mbedtls_gcm_free(&gcm);
  memset(key, 0, sizeof(key));
  return ok;
}

#if CAGI_VERIFIED_SKU
// ── Ed25519 (verified SKU) ───────────────────────────────────────────────────────────────────────
// Ed25519 keeps the SAME signature the platform's vc.ts produces, so a manifest signed here verifies
// under verifyManifest(). Two custodies (see crypto.h). The software path uses mbedtls' Ed25519 (via
// the Everest impl shipped with recent mbedtls builds); if a given Arduino-ESP32 mbedtls lacks it,
// swap in a vendored ref implementation (e.g. orlp/ed25519) — the interface below does not change.

String ed25519PubKeyHex(const uint8_t seed32[32]) {
#if CAGI_SECURE_ELEMENT
  (void)seed32;
  // TODO(hardware): read the ATECC608's stored public key for the manifest signing slot and hex it.
  //   atcab_get_pubkey(SLOT, pub64); return toHex(pub64, 32-of-the-64 per the Ed25519 encoding);
  return String();  // stub until the SE lib is wired
#else
  // TODO(driver): derive the Ed25519 public key from the seed. With mbedtls Ed25519:
  //   mbedtls_ed25519 keypair from seed → export 32-byte public key → toHex(pub, 32).
  // Kept as a documented stub so the verified build links without pinning a specific crypto lib.
  (void)seed32;
  return String();
#endif
}

String ed25519SignHex(const uint8_t* msg, size_t len, const uint8_t seed32[32]) {
#if CAGI_SECURE_ELEMENT
  (void)seed32;
  // TODO(hardware): hand `msg`/`len` to the SE to sign with its protected Ed25519 key (the key never
  // enters ESP32 RAM). atcab_sign / the part's Ed25519 op → 64-byte sig → toHex(sig, 64).
  (void)msg;
  (void)len;
  return String();  // stub until the SE lib is wired
#else
  // TODO(driver): software Ed25519 sign over the raw message bytes with `seed32`:
  //   ed25519_sign(sig64, msg, len, pub, priv-from-seed); return toHex(sig64, 64).
  (void)msg;
  (void)len;
  (void)seed32;
  return String();  // stub until the crypto lib is wired
#endif
}
#endif  // CAGI_VERIFIED_SKU

}  // namespace Crypto
