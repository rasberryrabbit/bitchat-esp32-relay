/*
 * peer_keys.h - senderID(8B) → Ed25519 공개키(32B) 매핑
 *
 * 한국어:
 *   bitchat 패킷의 senderID는 8B이지만, 서명 검증에는 32B Ed25519 공개키가 필요.
 *   ANNOUNCE 메시지의 payload에서 32B pk를 추출하여 매핑 저장.
 *   이후 MESSAGE 수신 시 senderID로 pk를 조회하여 서명 검증.
 *
 *   NVS 저장: "bpkey" namespace에 blob으로 저장
 *   - key: 8B senderID → value: 32B Ed25519 pk
 *   - 최대 32개 매핑 (32 × 40B = 1.28KB)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC_PEER_KEY_MAX_ENTRIES  32
#define BC_PEER_KEY_ID_SIZE       8
#define BC_PEER_KEY_PK_SIZE       32

// 초기화
bool peer_keys_init(void);

// 매핑 추가/갱신 (senderID 8B → pk 32B)
// NVS에 자동 저장. 이미 있으면 갱신.
bool peer_keys_add(const uint8_t* sender_id, const uint8_t* pk);

// 매핑 조회 (senderID 8B → pk 32B)
// 성공 시 true, 매핑 없으면 false
bool peer_keys_get(const uint8_t* sender_id, uint8_t* out_pk);

// 매핑 개수
size_t peer_keys_count(void);

// 전체 삭제
void peer_keys_clear(void);

// 디버그 출력
void peer_keys_debug_dump(void);

#ifdef __cplusplus
}
#endif
