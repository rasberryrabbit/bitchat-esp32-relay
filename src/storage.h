/*
 * storage.h - NVS persistence for keys / peer list / config
 *
 * 한국어:
 *   - 자체 Ed25519 키쌍을 최초 부팅 시 한 번 생성하고 NVS에 저장
 *   - 이후 부팅 때마다 NVS에서 복원
 *   - 피어 공개키 캐시 (선택 사항 - 화이트리스트 용도)
 *   - 카운터 / 통계 저장 (선택 사항)
 *
 * 자체 키 영속화 크기: 64B privkey + 32B pubkey = 96B
 *   (Ed25519는 결정론적 키가 아니므로 두 값을 모두 저장해야 함)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BC_NVS_NAMESPACE  "bchat"
#define BC_NVS_KEY_PRIV    "priv"     // 64-byte Ed25519 private key
#define BC_NVS_KEY_PUB    "pub"      // 32-byte Ed25519 public key
#define BC_NVS_KEY_COUNTER "ctr"
#define BC_NVS_KEY_PEERS  "peers"
#define BC_NVS_KEY_CONFIG "cfg"
#define BC_MAX_PEERS       32  // 최대 캐시 피어 수

typedef struct {
    uint8_t  public_key[32];
    uint8_t  reserved[32];   // 닉네임/메타정보 확장용
} bitchat_peer_t;

typedef struct {
    bool           ready;
    bitchat_keypair_t self_kp;       // 자체 키 (RAM 캐시)
    uint64_t       self_counter;     // 패킷 counter
    bitchat_peer_t peers[BC_MAX_PEERS];
    uint8_t        peer_count;
} bitchat_storage_t;

// 최초 1회 호출: NVS open + 키 복원/생성
bool storage_init(bitchat_storage_t* st);

// 자체 키 쌍 반환 (영속화된 키)
const bitchat_keypair_t* storage_self_keypair(const bitchat_storage_t* st);

// 카운터 1 증가 + NVS 동기화. 새 패킷 송신 시마다 호출
uint64_t storage_next_counter(bitchat_storage_t* st);

// 새 피어 공개키 추가 (중복이면 false 반환)
bool storage_add_peer(bitchat_storage_t* st, const uint8_t* pk);

// 피어 목록에서 공개키 검색
bool storage_has_peer(const bitchat_storage_t* st, const uint8_t* pk);

// 디버그용 - Serial에 저장 상태 출력
void storage_debug_dump(const bitchat_storage_t* st);

#ifdef __cplusplus
}
#endif
