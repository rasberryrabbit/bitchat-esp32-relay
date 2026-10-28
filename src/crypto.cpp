/*
 * crypto.cpp - Ed25519 implementation using libsodium
 *
 * 한국어:
 *   arduino-esp32 v2.x에 포함된 libsodium (libsodium.a)을 사용하여
 *   Ed25519 키쌍 생성 / 서명 / 검증을 수행.
 *
 *   - libsodium API: crypto_sign_keypair, crypto_sign_detached, crypto_sign_verify_detached
 *   - sodium_init() 최초 1회 호출 필요
 *   - 모든 함수는 thread-safe (sodium 자체가 thread-safe)
 */

#include "crypto.h"
#include "log.h"

#include <sodium.h>
#include <string.h>

static bool g_init_done = false;

void crypto_init(void) {
    if (g_init_done) return;
    int rc = sodium_init();
    if (rc < 0) {
        LOGE("sodium_init failed: %d", rc);
    } else {
        // rc == 0: 첫 호출, rc == 1: 이미 초기화됨
        LOGI("crypto: libsodium ready (Ed25519, %u/%u/%u bytes pk/sk/sig)",
             (unsigned)crypto_sign_PUBLICKEYBYTES,
             (unsigned)crypto_sign_SECRETKEYBYTES,
             (unsigned)crypto_sign_BYTES);
    }
    g_init_done = true;
}

bool crypto_generate_keypair(bitchat_keypair_t* kp) {
    if (!g_init_done) crypto_init();
    if (!kp) return false;
    memset(kp, 0, sizeof(*kp));

    // libsodium crypto_sign_keypair - pk 32B, sk 64B (seed+pk)
    int rc = crypto_sign_keypair(kp->public_key, kp->private_key);
    if (rc != 0) {
        LOGE("crypto_sign_keypair failed: %d", rc);
        return false;
    }
    kp->ready = true;
    return true;
}

bool crypto_keypair_from_storage(bitchat_keypair_t* kp,
                                 const uint8_t privkey[64],
                                 const uint8_t pubkey[32]) {
    if (!kp || !privkey || !pubkey) return false;
    memset(kp, 0, sizeof(*kp));
    memcpy(kp->private_key, privkey, 64);
    memcpy(kp->public_key,  pubkey,  32);
    kp->ready = true;
    return true;
}

bool crypto_verify_signature(const uint8_t* public_key, size_t pk_len,
                             const uint8_t* message, size_t msg_len,
                             const uint8_t* signature, size_t sig_len) {
    if (!g_init_done) crypto_init();
    if (!public_key || !message || !signature) return false;
    if (pk_len != crypto_sign_PUBLICKEYBYTES) return false;  // 32
    if (sig_len != crypto_sign_BYTES) return false;          // 64
    // libsodium은 verify 실패 시 -1 반환, 성공 시 0
    int rc = crypto_sign_verify_detached(signature,
                                          (const unsigned char*)message,
                                          (unsigned long long)msg_len,
                                          public_key);
    if (rc != 0) {
        LOGD("ed25519 verify fail: %d", rc);
        return false;
    }
    return true;
}

bool bitchat_sign(const bitchat_keypair_t* kp,
                  const uint8_t* message, size_t msg_len,
                  uint8_t* out_signature, size_t sig_max) {
    if (!g_init_done) crypto_init();
    if (!kp || !message || !out_signature) return false;
    if (!kp->ready) return false;
    if (sig_max < crypto_sign_BYTES) return false;  // 64

    unsigned long long sig_len_out = 0;
    int rc = crypto_sign_detached(out_signature,
                                   &sig_len_out,
                                   (const unsigned char*)message,
                                   (unsigned long long)msg_len,
                                   kp->private_key);
    if (rc != 0) {
        LOGE("crypto_sign_detached failed: %d", rc);
        return false;
    }
    return (sig_len_out == crypto_sign_BYTES);
}
