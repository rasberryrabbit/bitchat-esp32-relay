/*
 * packet_splitter.h - 큰 bitchat 패킷 분할/재조립
 *
 * 한국어:
 *   LoRa 프레임 한계 222B로 큰 bitchat 패킷(최대 623B)이 drop되는 문제 해결.
 *   원본 패킷을 222B 이하 청크로 분할하여 순차 송신 → 수신 측에서 재조립.
 *
 *   청크 포맷 (14 bytes header + payload):
 *   +-------+------+----------+----------+-----------+
 *   | magic | ver  | total    | index    | hash(8B)   |
 *   | 0xCF  | 0x01 | chunks   | (0-based)| (sender_pk  |
 *   |       |      | (1..16)  |          | [0..7] XOR  |
 *   |       |      |          |          | counter BE)  |
 *   +-------+------+----------+----------+-----------+
 *   | payload_len (2B)                                  |
 *   +----------------------------------------------------+
 *   | payload (chunk_payload_max bytes)                  |
 *   +----------------------------------------------------+
 *
 *   동작:
 *     송신: splitter_send_chunks(packet, len, tx_fn) - 분할 후 순차 tx_fn 호출
 *     수신: splitter_receive_chunk(chunk, len, out_packet, out_len)
 *           - hash 기준으로 16개 슬롯 중 하나에 저장
 *           - 모든 청크 도착 시 원본 패킷 복원, out_len 반환
 *           - 30초 타임아웃 - 미완료 시 drop
 *
 *   한계:
 *     - 16개 동시 재조립 슬롯 (16 × 623B = 10KB RAM)
 *     - 청크 순서 보장 안 함 - 먼저 온 청크부터 처리, 비트맵으로 추적
 *     - 단일 링크에서만 동작 (BLE/MQTT는 이미 자체 분할)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC_CHUNK_MAGIC          0xCF
#define BC_CHUNK_VERSION        0x01
#define BC_CHUNK_HEADER_LEN     14    // magic + ver + total + index + hash(8) + payload_len(2)
#define BC_MAX_CHUNKS_PER_PACKET 16

// LoRa 프레임 한계 222B → 청크 페이로드 = 222 - 14 = 208B
#ifndef BC_CHUNK_PAYLOAD_MAX
#define BC_CHUNK_PAYLOAD_MAX    208
#endif

// 재조립 타임아웃 (30초)
#define BC_CHUNK_TIMEOUT_MS     30000

// 전송 콜백 타입 - 각 청크마다 호출
// 예: LoRa TX (LoRa.beginPacket/write/endPacket)
typedef bool (*chunk_tx_fn)(const uint8_t* chunk, size_t len, void* user_data);

// 초기화 - 16개 재조립 슬롯 할당
bool splitter_init(void);

// 큰 패킷을 청크로 분할하여 송신
// packet: 원본 bitchat 패킷 (binary)
// packet_len: 길이
// tx: 각 청크 송신 시 호출되는 콜백
// user_data: 콜백에 전달할 사용자 데이터 (예: lora_relay 인스턴스)
// 반환: 모든 청크 송신 성공 시 true
bool splitter_send_chunks(const uint8_t* packet, size_t packet_len,
                          chunk_tx_fn tx, void* user_data);

// 청크 수신 - 한 청크 처리
// chunk: 수신한 청크 (BC_CHUNK_MAGIC 0xCF로 시작)
// chunk_len: 청크 길이
// out_packet: 재조립된 원본 패킷 저장 버퍼 (호출자가 BITCHAT_MAX_PKT_LEN 할당)
// out_len: 재조립 완료 시 원본 패킷 길이 저장
// 반환: true = 재조립 완료 (out_packet 사용 가능), false = 아직 더 받아야 함
bool splitter_receive_chunk(const uint8_t* chunk, size_t chunk_len,
                             uint8_t* out_packet, size_t* out_len);

// 주어진 버퍼가 청크인지 (magic 0xCF) 확인 - lora_relay에서 분기 처리용
bool splitter_is_chunk(const uint8_t* data, size_t len);

// 타임아웃된 미완료 슬롯 정리 - main loop에서 주기적 호출
void splitter_cleanup_timeouts(void);

// 디버그 - 현재 재조립 중인 슬롯 상태 출력
void splitter_debug_dump(void);

#ifdef __cplusplus
}
#endif
