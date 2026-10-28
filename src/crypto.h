/*
 * crypto.h - Ed25519 key generation / verification (libsodium-based)
 *
 * 한국어:
 *   - 자체 Ed25519 키쌍 생성 (RAM에만 보관)
 *   - Ed25519 서명 검증 (bitchat 패킷 검증용)
 *   - Ed25519 서명 생성 (relay 노드가 자체 메시지 송신 시, 향후 확장용)
 *   - 자체 키는 storage 모듈이 NVS로 영속화
 *
 * 백엔드: arduino-esp32 v2.x에 포함된 libsodium (libsodium/src/libsodium)
 *   - ESP32 클래식 + C3 모두 liblibsodium.a 링크됨
 *   - crypto_sign_KEYBYTES=32, SIGNBYTES=64, PUBLICKEYBYTES=32, SECRETKEYBYTES=64
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Ed25519 키 구조 (libsodium과 호환)
//   private_key: 64바이트 libsodium SK (seed[32] || pubkey[32])
//   public_key: 32바이트
typedef struct {
    uint8_t private_key[64];   // Ed25519 libsodium secret key (64B)
    uint8_t public_key[32];    // Ed25519 public key (32B)
    bool    ready;
} bitchat_keypair_t;

// libsodium 초기화 (한 번만 호출, setup에서)
void crypto_init(void);

// 자체 Ed25519 키쌍 새로 생성 (libsodium의 crypto_sign_keypair 사용)
bool crypto_generate_keypair(bitchat_keypair_t* kp);

// NVS에서 불러온 64B priv + 32B pub로 키쌍 복원
bool crypto_keypair_from_storage(bitchat_keypair_t* kp,
                                 const uint8_t privkey[64],
                                 const uint8_t pubkey[32]);

// Ed25519 서명 검증
// public_key: 32B, message: signing message, signature: 64B
bool crypto_verify_signature(const uint8_t* public_key, size_t pk_len,
                             const uint8_t* message, size_t msg_len,
                             const uint8_t* signature, size_t sig_len);

// 자체 키로 메시지에 서명 (relay 노드가 자체 메시지 송신 시 사용)
// 주의: libsodium이 `crypto_sign`이라는 C 함수를 global namespace에 정의하므로,
//       이 함수는 `bitchat_sign`으로 네이밍 변경
bool bitchat_sign(const bitchat_keypair_t* kp,
                  const uint8_t* message, size_t msg_len,
                  uint8_t* out_signature, size_t sig_max);
