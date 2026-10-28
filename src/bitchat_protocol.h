/*
 * bitchat_protocol.h - bitchat wire packet definition
 *
 * 한국어: bitchat 패킷의 이진 포맷 정의 + 직렬화/역직렬화 함수들.
 *
 * 패킷 레이아웃(빅엔디언):
 *   +-----+-----+-----+-----+------------------------+
 *   |magic| ver |type | ttl | sender_pk (32 bytes)   |
 *   +-----+-----+-----+-----+------------------------+
 *   | counter (8 bytes, msb first)                     |
 *   +--------------------------------------------------+
 *   | payload_len (2 bytes, big endian)                |
 *   +--------------------------------------------------+
 *   | payload (variable, 0..MAX_PAYLOAD_LEN)           |
 *   +--------------------------------------------------+
 *   | signature (64 bytes, Ed25519 over header+payload)|
 *   +--------------------------------------------------+
 *
 * 검증 규칙:
 *   - magic == 0xBC
 *   - ver   == 1   (현재 릴리즈)
 *   - ttl   <= 7   (메시 무한 루프 방지)
 *   - payload_len <= BITCHAT_MAX_PAYLOAD_LEN
 *   - signature must verify against sender_pk over the
 *     canonical header+counter+payload_len+payload byte stream
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define BITCHAT_MAGIC            0xBC
#define BITCHAT_VERSION          0x01
#define BITCHAT_MAX_TTL          7
#define BITCHAT_MAX_PAYLOAD_LEN  512

// Ed25519 key & signature sizes (RFC 8032)
#define BITCHAT_PUBKEY_LEN       32
#define BITCHAT_SIG_LEN          64

// 노드 식별자 - 공개키의 처음 6바이트 hex를 사람 친화적 ID로 사용
#define BITCHAT_ID_STR_LEN       13   // 12 hex chars + NUL

// 패킷 타입
enum BitchatPacketType : uint8_t {
    BC_TYPE_CHAT         = 0x01, // 암호화된 채팅 메시지 (ECDH + AES-GCM)
    BC_TYPE_ACK          = 0x02, // 수신 확인
    BC_TYPE_PEER_INFO    = 0x03, // 피어 발견 알림 (닉네임/공개키)
    BC_TYPE_PEER_REQUEST = 0x04, // 피어 목록 요청
    BC_TYPE_LEAVE        = 0x05, // 메시 이탈
    BC_TYPE_KEY_EXCHANGE = 0x06, // ECDH 공개키 교환
    BC_TYPE_PING         = 0x07, // keep-alive
    BC_TYPE_RESERVED     = 0xFF
};

// 고정 헤더 크기 (magic, ver, type, ttl, sender_pk, counter, payload_len)
#define BITCHAT_HEADER_LEN  (1 + 1 + 1 + 1 + BITCHAT_PUBKEY_LEN + 8 + 2)
// 최소 패킷 크기 = 헤더 + 빈 페이로드 + 서명
#define BITCHAT_MIN_PKT_LEN (BITCHAT_HEADER_LEN + BITCHAT_SIG_LEN)
// 최대 패킷 크기 = 헤더 + 최대 페이로드 + 서명
#define BITCHAT_MAX_PKT_LEN (BITCHAT_HEADER_LEN + BITCHAT_MAX_PAYLOAD_LEN + BITCHAT_SIG_LEN)

// 패킷을 파싱한 결과 (heap 할당 없음, caller가 제공한 버퍼 참조)
typedef struct {
    uint8_t  magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  ttl;
    const uint8_t* sender_pk;   // 32 bytes
    uint64_t counter;
    uint16_t payload_len;
    const uint8_t* payload;     // payload_len bytes
    const uint8_t* signature;   // 64 bytes (마지막 필드)
} bitchat_packet_view_t;

// 자체 패킷 빌드를 위한 writer 구조체 - 헤더를 순서대로 채운 뒤 payload, sig 추가
typedef struct {
    uint8_t  buffer[BITCHAT_MAX_PKT_LEN];
    size_t   used;
} bitchat_packet_builder_t;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// 버퍼를 패킷으로 파싱. 실패 시 false 반환.
// 성공 시 *view 필드가 버퍼 내부를 가리킴 (caller가 소유권 유지).
bool bitchat_parse(const uint8_t* buf, size_t len, bitchat_packet_view_t* view);

// 전체 패킷 크기 계산 (헤더 + 페이로드 + 서명)
size_t bitchat_total_len(const bitchat_packet_view_t* v);

// 패킷을 Serial에 hex+구조화된 텍스트로 출력 (log.h 의존)
void bitchat_packet_debug(const bitchat_packet_view_t* v);

// 서명 메시지를 계산: (magic,ver,type,ttl,sender_pk,counter,payload_len,payload)
// 결과는 out_buf 에 기록. out_len 바이트 기록됨. out_buf는 최소
// BITCHAT_HEADER_LEN + payload_len 크기여야 함.
void bitchat_signing_message(const bitchat_packet_view_t* v,
                             uint8_t* out_buf, size_t* out_len);

// 사람 친화적 노드 ID를 hex로 생성
// pk(32B)의 앞 6바이트 -> 12 hex chars (null-terminated)
void bitchat_id_from_pk(const uint8_t* pk, char* out_id);

// ESP32의 MAC 주소를 pk 대신 임시 발급 키로 사용할 때 보조용
// (실제 키는 storage/crypto 모듈에서 영속화)
void bitchat_id_from_mac(const uint8_t* mac, char* out_id);

// 패킷 헤더 빌더 초기화 (sender_pk, type, payload 채우면 헤더까지 완성)
// 반환값: 사용된 헤더 길이, 실패 시 0
size_t bitchat_build_header(bitchat_packet_builder_t* b,
                            uint8_t type, uint8_t ttl,
                            const uint8_t* sender_pk, uint32_t counter,
                            const uint8_t* payload, uint16_t payload_len);

// 빌더 끝에 64바이트 서명 필드 자리만 남겨두고 (실제 서명은 crypto 모듈이 채움)
// 반환값: 서명이 들어갈 오프셋 (실패 시 0)
size_t bitchat_builder_reserve_sig(bitchat_packet_builder_t* b);

// 패킷 TTL을 1 감소시키고 다시 직렬화 (중계 시 사용)
// src는 전체 패킷, src_len은 길이, dst는 출력 버퍼
// 반환: dst에 기록된 바이트 수, 0이면 실패
size_t bitchat_decrement_ttl(const uint8_t* src, size_t src_len,
                             uint8_t* dst, size_t dst_max);
