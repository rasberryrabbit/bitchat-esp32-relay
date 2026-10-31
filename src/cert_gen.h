/*
 * cert_gen.h - Self-signed certificate auto-generation (mbedTLS)
 *
 * v25: Generates a self-signed CA + client cert/key pair on the ESP32
 *   at runtime using mbedTLS. Saves to SPIFFS as PEM files.
 *   RSA 2048-bit key (takes ~10-30s on ESP32 classic).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Generate self-signed CA + client certificate + private key.
// All three are written to SPIFFS:
//   /broker_ca.pem    - self-signed CA certificate
//   /client_cert.pem  - client certificate signed by CA
//   /client_key.pem   - client RSA private key
//
// cn: Common Name for the certificates (e.g. "bitchat-relay")
//     If NULL or empty, uses "bitchat-relay-{chip_id}".
//
// Returns true on success.
// This function blocks for 10-30 seconds (RSA key generation).
bool cert_generate_self_signed(const char* cn);

#ifdef __cplusplus
}
#endif
