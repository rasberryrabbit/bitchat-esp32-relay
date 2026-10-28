/*
 * bitchat_protocol.h - 실제 bitchat 호환 패킷 포맷 (BinaryProtocol.kt 기반)
 *
 * 한국어:
 *   permissionlesstech/bitchat-android의 BinaryProtocol.kt에서 추출한
 *   와이어 포맷 100% 호환.
 *
 *   패킷 레이아웃 (v1, 14-byte header):
 *
 *   +------+-------+------+---------------------+
 *   |ver   | type  | ttl  | timestamp (8B BE)    |
 *   | 1B   | 1B    | 1B   |                      |
 *   +------+-------+------+---------------------+
 *   |flags | payload_len (2B BE, v1)            |
 *   | 1B   |                                       |
 *   +------+---------------------------------------+
 *
 *   Variable sections (순서대로):
 *   - senderID: 8 bytes (항상)
 *   - recipientID: 8 bytes (flags & 0x01)
 *   - route: 1B count + N×8B (v2+만, flags & 0x08)
 *   - payload: 가변 길이 (압축 시 앞에 originalSize 2B/4B)
 *   - signature: 64 bytes (flags & 0x02)
 *
 *   Message Types:
 *     0x01 ANNOUNCE - peer discovery
 *     0x02 MESSAGE  - 모든 사용자 메시지 (private + broadcast)
 *     0x03 LEAVE
 *     0x10 NOISE_HANDSHAKE - Noise protocol handshake
 *     0x11 NOISE_ENCRYPTED - Noise encrypted transport
 *     0x20 FRAGMENT - 큰 패킷 분할
 *     0x21 REQUEST_SYNC - GCS-based sync request
 *     0x22 FILE_TRANSFER - 파일 전송
 *     0x29 VOICE_FRAME - push-to-talk frame
 *
 *   Flags (bit field):
 *     0x01 HAS_RECIPIENT
 *     0x02 HAS_SIGNATURE
 *     0x04 IS_COMPRESSED
 *     0x08 HAS_ROUTE (v2+ only)
 *
 *   서명: Ed25519 over (encoded packet, signature 없이, TTL=0으로)
 *         → TTL은 relay 중 변경되지만 서명은 유효함
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BITCHAT_VERSION_1        1
#define BITCHAT_VERSION_2        2

#define BITCHAT_HEADER_V1_LEN    14   // ver(1)+type(1)+ttl(1)+ts(8)+flags(1)+plen(2)
#define BITCHAT_HEADER_V2_LEN    16   // 위와 동일, payload_len만 4B

#define BITCHAT_SENDER_ID_SIZE   8    // peerID 8 bytes (Ed25519 pk 아님)
#define BITCHAT_RECIPIENT_ID_SIZE 8
#define BITCHAT_SIGNATURE_SIZE   64
#define BITCHAT_BROADCAST_ID     0xFF  // 8바이트 모두 0xFF = broadcast

#define BITCHAT_MAX_PAYLOAD_LEN  650  // v2에서 약간 더 큰 payload 허용

// 패킷 사이즈 한계 (header + 8B sender + 8B recipient + payload + 64B sig)
#define BITCHAT_MAX_PKT_LEN      (BITCHAT_HEADER_V1_LEN + BITCHAT_SENDER_ID_SIZE + \
                                  BITCHAT_RECIPIENT_ID_SIZE + BITCHAT_MAX_PAYLOAD_LEN + \
                                  BITCHAT_SIGNATURE_SIZE + 64)   // padding slack

// 최소 패킷 길이 (header + senderID, 서명 없는 최소)
#define BITCHAT_MIN_PKT_LEN      (BITCHAT_HEADER_V1_LEN + BITCHAT_SENDER_ID_SIZE)

// 하위 호환 alias (기존 BITCHAT_TYPE_*, PUBKEY_LEN, SIG_LEN, ID_STR_LEN)
#define BITCHAT_PUBKEY_LEN       BITCHAT_SENDER_ID_SIZE      // 8 bytes (실제로는 senderID)
#define BITCHAT_SIG_LEN          BITCHAT_SIGNATURE_SIZE       // 64 bytes
#define BITCHAT_ID_STR_LEN       (BITCHAT_SENDER_ID_SIZE * 2 + 1)  // 17

// Legacy type aliases (기존 코드와 호환)
#define BITCHAT_TYPE_CHAT            BITCHAT_MSG_MESSAGE
#define BITCHAT_TYPE_ACK             BITCHAT_MSG_MESSAGE       // bitchat에 ACK 없음, MESSAGE로 매핑
#define BITCHAT_TYPE_PEER_INFO       BITCHAT_MSG_ANNOUNCE
#define BITCHAT_TYPE_PEER_REQUEST    BITCHAT_MSG_REQUEST_SYNC
#define BITCHAT_TYPE_LEAVE           BITCHAT_MSG_LEAVE
#define BITCHAT_TYPE_KEY_EXCHANGE    BITCHAT_MSG_NOISE_HANDSHAKE
#define BITCHAT_TYPE_PING            BITCHAT_MSG_ANNOUNCE       // PING 대신 ANNOUNCE 사용

// Message types
enum BitchatType : uint8_t {
    BITCHAT_MSG_ANNOUNCE         = 0x01,
    BITCHAT_MSG_MESSAGE          = 0x02,
    BITCHAT_MSG_LEAVE            = 0x03,
    BITCHAT_MSG_NOISE_HANDSHAKE  = 0x10,
    BITCHAT_MSG_NOISE_ENCRYPTED  = 0x11,
    BITCHAT_MSG_FRAGMENT         = 0x20,
    BITCHAT_MSG_REQUEST_SYNC     = 0x21,
    BITCHAT_MSG_FILE_TRANSFER    = 0x22,
    BITCHAT_MSG_VOICE_FRAME      = 0x29,
};

// Flags
#define BITCHAT_FLAG_HAS_RECIPIENT  0x01
#define BITCHAT_FLAG_HAS_SIGNATURE  0x02
#define BITCHAT_FLAG_IS_COMPRESSED  0x04
#define BITCHAT_FLAG_HAS_ROUTE      0x08

// 파싱된 패킷 뷰 (원본 버퍼 참조, heap 할당 없음)
typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint8_t  ttl;
    uint64_t timestamp;
    uint8_t  flags;
    uint32_t payload_len;
    const uint8_t* sender_id;     // 8 bytes
    const uint8_t* recipient_id; // 8 bytes (또는 NULL)
    const uint8_t* route;        // 1B count + N×8B (또는 NULL)
    uint8_t  route_count;
    const uint8_t* payload;       // payload_len bytes
    const uint8_t* signature;     // 64B (또는 NULL)
    size_t   total_len;
} bitchat_packet_view_t;

// 패킷 빌더
typedef struct {
    uint8_t  buffer[BITCHAT_MAX_PKT_LEN];
    size_t   used;
} bitchat_packet_builder_t;

// 패킷 파싱
bool bitchat_parse(const uint8_t* buf, size_t len, bitchat_packet_view_t* view);

// 서명 검증용 메시지 추출 (TTL=0으로 인코딩, signature 필드 제외)
// out_buf: 최소 BITCHAT_MAX_PKT_LEN 크기
// 반환: signing message 길이, 0이면 실패
size_t bitchat_signing_message(const bitchat_packet_view_t* v,
                                uint8_t* out_buf, size_t out_max);

// TTL 1 감소 후 패킷 재인코딩 (relay가 forward 시 사용)
// src: 원본 패킷, src_len: 길이, dst: 출력 버퍼
// 반환: dst에 기록된 바이트 수, 0이면 실패
size_t bitchat_decrement_ttl(const uint8_t* src, size_t src_len,
                              uint8_t* dst, size_t dst_max);

// 8바이트 senderID를 hex 문자열로 (16 chars + NUL)
void bitchat_id_from_sender(const uint8_t* sender_id, char* out_id);

// 하위 호환 alias (기존 bitchat_id_from_pk 호출 유지)
#define bitchat_id_from_pk(sender_id, out_id) bitchat_id_from_sender(sender_id, out_id)

// 패킷 타입 이름 (디버그)
const char* bitchat_type_name(uint8_t type);

// flags 이름 문자열 (디버그)
void bitchat_flags_str(uint8_t flags, char* out);

// broadcast ID 체크
bool bitchat_is_broadcast(const uint8_t* recipient_id);

#ifdef __cplusplus
}
#endif
