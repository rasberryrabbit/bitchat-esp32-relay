/*
 * peer_keys.h - senderID(8B) → Ed25519 공개키(32B) 매핑
 *
 * 한국어:
 *   bitchat 패킷의 senderID는 8B이지만, 서명 검증에는 32B Ed25519 공개키가 필요.
 *   ANNOUNCE 메시지의 payload에서 32B pk를 추출하여 매핑 저장.
 *   이후 MESSAGE 수신 시 senderID로 pk를 조회하여 서명 검증.
 *
 *   저장 백엔드: SPIFFS 파일 /peer_keys.bin (이전에는 NVS blob 사용)
 *     - 단일 파일, 고정 크기 레코드
 *     - 32개 매핑 × (1+8+64+1) = 32 × 74B = 2,368B
 *     - 부팅 시 1회 로드 → RAM 캐시, 이후 빠른 조회
 *
 *   SPIFFS 전환 이유:
 *     - NVS blob 한계 (~4KB) → 32개가 최대
 *     - NVS namespace "bpkey" → SPIFFS로 1회 자동 마이그레이션
 *     - 파일 단위 저장 → 다른 설정 (node.conf, wifi.conf)과 일관성
 *
 *   Serial 명령 (main.cpp):
 *     peer list        - 현재 매핑 dump
 *     peer clear       - 전체 삭제
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

// SPIFFS 파일 경로
#define BC_PEER_KEYS_FILE        "/peer_keys.bin"

// 매핑 추가 (key_index: 0=primary, 1=secondary)
// 두 키를 시도해야 하므로 인덱스 지정 가능
bool peer_keys_add_indexed(const uint8_t* sender_id, const uint8_t* pk, int key_index);

// 매핑 조회 (key_index: 0 또는 1)
bool peer_keys_get_indexed(const uint8_t* sender_id, int key_index, uint8_t* out_pk);

// 초기화 - SPIFFS 마운트 + /peer_keys.bin 로드
// NVS에 기존 매핑이 있으면 1회 자동 마이그레이션 → SPIFFS로 이관 후 NVS 삭제
bool peer_keys_init(void);

// 매핑 추가/갱신 (senderID 8B → pk 32B)
// SPIFFS에 자동 저장. 이미 있으면 갱신.
bool peer_keys_add(const uint8_t* sender_id, const uint8_t* pk);

// 매핑 조회 (senderID 8B → pk 32B)
// 성공 시 true, 매핑 없으면 false
bool peer_keys_get(const uint8_t* sender_id, uint8_t* out_pk);

// 매핑 개수
size_t peer_keys_count(void);

// 전체 삭제 (SPIFFS 파일 제거)
void peer_keys_clear(void);

// 디버그 출력
void peer_keys_debug_dump(void);

#ifdef __cplusplus
}
#endif
