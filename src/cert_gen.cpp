/*
 * cert_gen.cpp - Self-signed certificate auto-generation using mbedTLS
 *
 * v25: Generates CA + client cert + private key at runtime.
 *   RSA 2048-bit, SHA-256, valid 10 years.
 *   Saves to SPIFFS as PEM files.
 *   v25-fix: single PEM buffer (reuse), free CA cert ctx after write,
 *            fix sizeof(pointer) bug, heap conservation.
 */

#include "cert_gen.h"
#include "log.h"

#include <Arduino.h>
#include <SPIFFS.h>

#include <mbedtls/rsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/bignum.h>
#include <string.h>

#define CERT_KEY_BITS    2048
#define PEM_BUF_SIZE     4096

static bool write_pem_file(const char* path, const unsigned char* pem, size_t len) {
    File f = SPIFFS.open(path, "w");
    if (!f) {
        LOGE("cert_gen: failed to open %s", path);
        return false;
    }
    size_t written = f.write(pem, len);
    f.close();
    if (written != len) {
        LOGE("cert_gen: short write %s (%u/%u)", path, (unsigned)written, (unsigned)len);
        return false;
    }
    LOGI("cert_gen: wrote %s (%u bytes)", path, (unsigned)len);
    return true;
}

bool cert_generate_self_signed(const char* cn) {
    char common_name[48];
    if (cn && cn[0]) {
        strncpy(common_name, cn, sizeof(common_name) - 1);
        common_name[sizeof(common_name) - 1] = 0;
    } else {
        uint64_t mac = ESP.getEfuseMac();
        snprintf(common_name, sizeof(common_name), "bitchat-relay-%04X",
                 (uint16_t)(mac >> 32));
    }

    char ca_dn[64], client_dn[72];
    snprintf(ca_dn, sizeof(ca_dn), "CN=%s", common_name);
    snprintf(client_dn, sizeof(client_dn), "CN=%s-client", common_name);

    LOGI("cert_gen: generating certs (CN=%s, RSA %d)...", common_name, CERT_KEY_BITS);
    LOGI("cert_gen: blocks ~10-30s for RSA key gen");

    mbedtls_pk_context ca_key, client_key;
    mbedtls_x509write_cert ca_crt, cli_crt;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_mpi serial;

    mbedtls_pk_init(&ca_key);
    mbedtls_pk_init(&client_key);
    mbedtls_x509write_crt_init(&ca_crt);
    mbedtls_x509write_crt_init(&cli_crt);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_mpi_init(&serial);

    // v25-fix: single reusable PEM buffer (was 3 × 4KB = 12KB, caused heap exhaustion)
    unsigned char* pem = (unsigned char*)malloc(PEM_BUF_SIZE);
    bool ok = false;
    int ret;

    if (!pem) {
        LOGE("cert_gen: malloc failed for PEM buffer");
        goto done_no_pem;
    }

    // Seed RNG
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char*)"bitchat", 7);
    if (ret != 0) { LOGE("cert_gen: drbg seed: -0x%04x", -ret); goto done; }

    ret = mbedtls_mpi_lset(&serial, 1);
    if (ret != 0) { LOGE("cert_gen: serial: -0x%04x", -ret); goto done; }

    // Generate CA key
    LOGI("cert_gen: generating CA key...");
    ret = mbedtls_pk_setup(&ca_key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0) { LOGE("cert_gen: CA pk_setup: -0x%04x", -ret); goto done; }
    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(ca_key),
                              mbedtls_ctr_drbg_random, &ctr_drbg,
                              CERT_KEY_BITS, 65537);
    if (ret != 0) { LOGE("cert_gen: CA rsa_gen: -0x%04x", -ret); goto done; }
    LOGI("cert_gen: CA key done");

    // Generate client key
    LOGI("cert_gen: generating client key...");
    ret = mbedtls_pk_setup(&client_key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0) { LOGE("cert_gen: cli pk_setup: -0x%04x", -ret); goto done; }
    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(client_key),
                              mbedtls_ctr_drbg_random, &ctr_drbg,
                              CERT_KEY_BITS, 65537);
    if (ret != 0) { LOGE("cert_gen: cli rsa_gen: -0x%04x", -ret); goto done; }
    LOGI("cert_gen: client key done");

    // Build CA cert (self-signed)
    LOGI("cert_gen: building CA certificate...");
    mbedtls_x509write_crt_set_md_alg(&ca_crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&ca_crt, &ca_key);
    mbedtls_x509write_crt_set_issuer_key(&ca_crt, &ca_key);
    ret = mbedtls_x509write_crt_set_subject_name(&ca_crt, ca_dn);
    if (ret != 0) { LOGE("cert_gen: CA subj: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_issuer_name(&ca_crt, ca_dn);
    if (ret != 0) { LOGE("cert_gen: CA issuer: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_serial(&ca_crt, &serial);
    if (ret != 0) { LOGE("cert_gen: CA serial: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_validity(&ca_crt, "20300101000000", "20400101000000");
    if (ret != 0) { LOGE("cert_gen: CA validity: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_basic_constraints(&ca_crt, 1, 1);
    if (ret != 0) { LOGE("cert_gen: CA bc: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_subject_key_identifier(&ca_crt);
    if (ret != 0) { LOGE("cert_gen: CA ski: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_key_usage(&ca_crt,
        MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN);
    if (ret != 0) { LOGE("cert_gen: CA ku: -0x%04x", -ret); goto done; }

    // Write CA PEM — use PEM_BUF_SIZE not sizeof(pem) (which is pointer size=4)
    memset(pem, 0, PEM_BUF_SIZE);
    ret = mbedtls_x509write_crt_pem(&ca_crt, pem, PEM_BUF_SIZE,
                                     mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret <= 0) { LOGE("cert_gen: CA PEM: -0x%04x", -ret); goto done; }
    {
        size_t ca_len = strlen((char*)pem);
        if (!write_pem_file("/broker_ca.pem", pem, ca_len)) goto done;
    }

    // v25-fix: free CA cert write context to reclaim heap for client cert
    mbedtls_x509write_crt_free(&ca_crt);
    mbedtls_x509write_crt_init(&ca_crt);  // re-init for safe cleanup

    // Build client cert (signed by CA key)
    LOGI("cert_gen: building client certificate...");
    mbedtls_x509write_crt_set_md_alg(&cli_crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cli_crt, &client_key);
    mbedtls_x509write_crt_set_issuer_key(&cli_crt, &ca_key);  // CA signs
    ret = mbedtls_x509write_crt_set_subject_name(&cli_crt, client_dn);
    if (ret != 0) { LOGE("cert_gen: cli subj: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_issuer_name(&cli_crt, ca_dn);
    if (ret != 0) { LOGE("cert_gen: cli issuer: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_serial(&cli_crt, &serial);
    if (ret != 0) { LOGE("cert_gen: cli serial: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_validity(&cli_crt, "20300101000000", "20400101000000");
    if (ret != 0) { LOGE("cert_gen: cli validity: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_basic_constraints(&cli_crt, 0, 0);
    if (ret != 0) { LOGE("cert_gen: cli bc: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_subject_key_identifier(&cli_crt);
    if (ret != 0) { LOGE("cert_gen: cli ski: -0x%04x", -ret); goto done; }
    ret = mbedtls_x509write_crt_set_key_usage(&cli_crt,
        MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT);
    if (ret != 0) { LOGE("cert_gen: cli ku: -0x%04x", -ret); goto done; }

    // Write client cert PEM — reuse same buffer
    memset(pem, 0, PEM_BUF_SIZE);
    ret = mbedtls_x509write_crt_pem(&cli_crt, pem, PEM_BUF_SIZE,
                                     mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret <= 0) { LOGE("cert_gen: cli PEM: -0x%04x", -ret); goto done; }
    {
        size_t cli_len = strlen((char*)pem);
        if (!write_pem_file("/client_cert.pem", pem, cli_len)) goto done;
    }

    // Free client cert write context to reclaim heap for key PEM
    mbedtls_x509write_crt_free(&cli_crt);
    mbedtls_x509write_crt_init(&cli_crt);

    // Write client private key PEM — reuse same buffer
    LOGI("cert_gen: writing client private key...");
    memset(pem, 0, PEM_BUF_SIZE);
    ret = mbedtls_pk_write_key_pem(&client_key, pem, PEM_BUF_SIZE);
    if (ret != 0) { LOGE("cert_gen: key PEM: -0x%04x", -ret); goto done; }
    {
        size_t key_len = strlen((char*)pem);
        if (!write_pem_file("/client_key.pem", pem, key_len)) goto done;
    }

    LOGI("cert_gen: OK - CA + client cert + key generated");
    LOGI("cert_gen: /broker_ca.pem /client_cert.pem /client_key.pem");
    LOGI("cert_gen: run 'net tls on' then 'net on' to use mTLS");
    ok = true;

done:
    free(pem);
done_no_pem:
    mbedtls_x509write_crt_free(&ca_crt);
    mbedtls_x509write_crt_free(&cli_crt);
    mbedtls_pk_free(&ca_key);
    mbedtls_pk_free(&client_key);
    mbedtls_mpi_free(&serial);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (!ok) LOGE("cert_gen: failed");
    return ok;
}
