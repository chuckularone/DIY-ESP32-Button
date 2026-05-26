#pragma once
/*
 * base64_util.h — Minimal base64 encoder (RFC 4648)
 * No external library. Encode binary → base64 string.
 */

#include <Arduino.h>

static const char B64_TABLE[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode `len` bytes of `input` into a base64 String (with '=' padding).
inline String base64Encode(const uint8_t* input, size_t len) {
  String out;
  out.reserve(((len + 2) / 3) * 4 + 1);

  for (size_t i = 0; i < len; i += 3) {
    uint8_t b0 = input[i];
    uint8_t b1 = (i + 1 < len) ? input[i + 1] : 0;
    uint8_t b2 = (i + 2 < len) ? input[i + 2] : 0;

    out += B64_TABLE[(b0 >> 2) & 0x3F];
    out += B64_TABLE[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)];
    out += (i + 1 < len) ? B64_TABLE[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : '=';
    out += (i + 2 < len) ? B64_TABLE[b2 & 0x3F] : '=';
  }
  return out;
}

// Generate 32 random bytes via mbedtls CSPRNG, return as base64 String.
// Suitable for use as an API key / shared secret.
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

inline String generateApiKey() {
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  uint8_t buf[32];

  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);

  const char* pers = "diybutton_keygen";
  mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                         (const unsigned char*)pers, strlen(pers));
  mbedtls_ctr_drbg_random(&ctr_drbg, buf, sizeof(buf));

  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);

  return base64Encode(buf, sizeof(buf));
}
