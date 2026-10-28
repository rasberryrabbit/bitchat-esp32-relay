/*
 * blacklist.h - 패킷 송신자/피어 차단 (블랙리스트)
 *
 * 한국어:
 *   두 가지 차단 메커니즘을 제공:
 *
 *   1) Ed25519 공개키 블랙리스트 (32B × 32개 슬롯)
 *      - bitchat 노드의 정식 식별자로 차단
 *      - 차단된 송신자의 패킷은 서명 검증 전에 drop
 *
 *   2) BLE 주소 블랙리스트 (6B × 16개 슬롯)
 *      - 낮은 수준의 차단 - 아예 연결을 맺지 않음
 *      - 광고 단계에서 필터링하여 BLE 자원 절약
 *
 *   모든 블랙리스트는 NVS에 영속화되어 재부팅 후에도 유지.
 *   Serial 명령으로 실시간 add/remove/list/clear 가능 (main.cpp 참고).
 *
 *   용량:
 *     pk 블랙리스트: 32 × 32B = 1KB
 *     BLE 블랙리스트: 16 × 6B = 96B
 *     총 NVS blob: ~1.1KB
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC_BL_MAX_PK_ENTRIES    32    // Ed25519 공개키 최대 저장 수
#define BC_BL_MAX_BLE_ENTRIES   16    // BLE 주소 최대 저장 수

// NVS blob 크기 (active flag 포함)
#define BC_BL_PK_BLOB_SIZE      (BC_BL_MAX_PK_ENTRIES * 33)  // 32B pk + 1B active flag
#define BC_BL_BLO_BLOB_SIZE     (BC_BL_MAX_BLE_ENTRIES * 7)   // 6B addr + 1B active flag

// 초기화 - NVS에서 블랙리스트 로드
// storage_init() 이후에 호출되어야 함
bool blacklist_init(void);

// 활성화/비활성화 - false면 모든 체크를 skip (블랙리스트 무시)
void blacklist_set_enabled(bool enabled);
bool blacklist_is_enabled(void);

// ----- Ed25519 공개키 블랙리스트 -----

// 공개키 추가 (이미 있거나 꽉 차면 false)
bool blacklist_add_pk(const uint8_t* pk);

// 공개키 제거
bool blacklist_remove_pk(const uint8_t* pk);

// 공개키가 블랙리스트에 있는지 확인
// (내부에서 binary search 또는 linear scan - 32개라 linear도 충분)
bool blacklist_is_pk_blocked(const uint8_t* pk);

// ----- BLE 주소 블랙리스트 -----

// BLE 주소 (6 bytes) 추가
bool blacklist_add_ble(const uint8_t* addr);

// BLE 주소 제거
bool blacklist_remove_ble(const uint8_t* addr);

// BLE 주소가 블랙리스트에 있는지 확인
bool blacklist_is_ble_blocked(const uint8_t* addr);

// ----- 유틸리티 -----

// 전체 초기화 (NVS에서도 삭제)
void blacklist_clear_all(void);

// 현재 카운트
size_t blacklist_get_pk_count(void);
size_t blacklist_get_ble_count(void);

// 디버그 출력
void blacklist_debug_dump(void);

// hex 문자열을 raw bytes로 변환 (Serial 명령 처리용)
// "8a3f2c1b9e04..." -> 32-byte pk 또는 6-byte addr
// 성공 시 변환된 바이트 수 반환 (32, 6, 또는 0)
size_t blacklist_hex_to_bytes(const char* hex, uint8_t* out, size_t out_max);

#ifdef __cplusplus
}
#endif
