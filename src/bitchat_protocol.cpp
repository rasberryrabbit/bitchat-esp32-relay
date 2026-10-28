/*
 * bitchat_protocol.cpp - 실제 bitchat 호환 패킷 (BinaryProtocol.kt 포팅)
 *
 * 한국어:
 *   permissionlesstech/bitchat-android의 BinaryProtocol.kt를 C++로 포팅.
 *   v1 (14B header, 2B payload_len) 지원. v2는 필요시 추가.
 *
 *   relay 노드 전략:
 *     - 패킷을 그대로 forward (TTL만 1 감소)
 *     - 서명은 TTL=0으로 고정 후 검증 (TTL 변경해도 서명 유효)
 *     - 압축/패딩/라우팅은 그대로 보존 (relay는 재인코딩 안 함)
 */

#include "bitchat_protocol.h"
#include "log.h"

#include <string.h>
#include <stdio.h>

// 빅엔디언 read/write helpers
static inline uint64_t rd_be_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
static inline uint32_t rd_be_u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint16_t rd_be_u16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline void wr_be_u64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}
static inline void wr_be_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v & 0xFF);
}
static inline void wr_be_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFF);
}

bool bitchat_parse(const uint8_t* buf, size_t len, bitchat_packet_view_t* view) {
    if (!buf || !view) return false;
    // 최소: header + senderID (8)
    if (len < BITCHAT_HEADER_V1_LEN + BITCHAT_SENDER_ID_SIZE) {
        return false;
    }

    uint8_t version = buf[0];
    if (version != BITCHAT_VERSION_1 && version != BITCHAT_VERSION_2) {
        return false;
    }

    size_t header_size = (version == BITCHAT_VERSION_1)
        ? BITCHAT_HEADER_V1_LEN
        : BITCHAT_HEADER_V2_LEN;

    uint8_t type    = buf[1];
    uint8_t ttl     = buf[2];
    uint64_t ts     = rd_be_u64(buf + 3);
    uint8_t flags   = buf[11];

    bool has_recipient = (flags & BITCHAT_FLAG_HAS_RECIPIENT) != 0;
    bool has_signature = (flags & BITCHAT_FLAG_HAS_SIGNATURE) != 0;
    bool is_compressed = (flags & BITCHAT_FLAG_IS_COMPRESSED) != 0;
    bool has_route     = (flags & BITCHAT_FLAG_HAS_ROUTE) != 0 &&
                         version >= BITCHAT_VERSION_2;

    // payload_len (v1: 2B, v2: 4B)
    uint32_t payload_len;
    size_t pl_off;
    if (version == BITCHAT_VERSION_1) {
        payload_len = rd_be_u16(buf + 12);
        pl_off = 14;
    } else {
        payload_len = rd_be_u32(buf + 12);
        pl_off = 16;
    }
    if (payload_len > BITCHAT_MAX_PAYLOAD_LEN) return false;

    // variable sections: senderID + (optional)recipientID + (optional)route + payload + (optional)sig
    size_t offset = pl_off;
    if (offset + BITCHAT_SENDER_ID_SIZE > len) return false;
    const uint8_t* sender_id = buf + offset;
    offset += BITCHAT_SENDER_ID_SIZE;

    const uint8_t* recipient_id = nullptr;
    if (has_recipient) {
        if (offset + BITCHAT_RECIPIENT_ID_SIZE > len) return false;
        recipient_id = buf + offset;
        offset += BITCHAT_RECIPIENT_ID_SIZE;
    }

    // Route (v2+): peek count first to compute size
    const uint8_t* route = nullptr;
    uint8_t route_count = 0;
    if (has_route) {
        if (offset + 1 > len) return false;
        route_count = buf[offset];
        route = buf + offset;
        offset += 1 + (route_count * BITCHAT_SENDER_ID_SIZE);
        if (offset > len) return false;
    }

    // Payload
    if (offset + payload_len > len) return false;
    const uint8_t* payload = buf + offset;
    offset += payload_len;

    // Signature
    const uint8_t* signature = nullptr;
    if (has_signature) {
        if (offset + BITCHAT_SIGNATURE_SIZE > len) return false;
        signature = buf + offset;
        offset += BITCHAT_SIGNATURE_SIZE;
    }

    view->version      = version;
    view->type         = type;
    view->ttl          = ttl;
    view->timestamp    = ts;
    view->flags        = flags;
    view->payload_len  = payload_len;
    view->sender_id    = sender_id;
    view->recipient_id = recipient_id;
    view->route        = route;
    view->route_count  = route_count;
    view->payload      = payload;
    view->signature    = signature;
    view->total_len    = offset;
    return true;
}

// 서명 검증용 메시지: 원본 패킷을 복사하되
//   - TTL을 0으로 설정
//   - signature 필드 제외 (그 뒤에 오는 데이터는 무시됨)
size_t bitchat_signing_message(const bitchat_packet_view_t* v,
                                uint8_t* out_buf, size_t out_max) {
    if (!v || !out_buf) return 0;

    size_t header_size = (v->version == BITCHAT_VERSION_1)
        ? BITCHAT_HEADER_V1_LEN
        : BITCHAT_HEADER_V2_LEN;

    // signing message = header(14/16B) + senderID(8) + recipientID(8 if) + route(if) + payload
    // signature는 제외
    size_t signing_len = header_size + BITCHAT_SENDER_ID_SIZE + v->payload_len;
    if (v->recipient_id) signing_len += BITCHAT_RECIPIENT_ID_SIZE;
    if (v->route) signing_len += 1 + (v->route_count * BITCHAT_SENDER_ID_SIZE);

    if (signing_len > out_max) return 0;

    // 원본 버퍼에서 signing_len 만큼 복사
    // (signature 앞까지가 signing_len)
    memcpy(out_buf, (const uint8_t*)(v) - (size_t)0, 0);  // dummy - 실제로는 v의 원본 버퍼 필요
    // 하지만 view에는 원본 버퍼 포인터가 없으므로, 우리는 별도 함수로 원본에서 바로 만들어야 함
    // 여기서는 인터페이스만 정의하고, 실제 구현은 ble_relay.cpp에서 원본 버퍼로 직접 처리
    return 0;   // 이 함수는 호출되지 않음 - signing은 ble_relay.cpp에서 직접 처리
}

// TTL 1 감소 - 패킷을 그대로 복사하되 TTL만 1 줄임
// 서명은 TTL 변경과 무관하게 유효 (TTL=0으로 서명했으므로)
size_t bitchat_decrement_ttl(const uint8_t* src, size_t src_len,
                              uint8_t* dst, size_t dst_max) {
    bitchat_packet_view_t v;
    if (!bitchat_parse(src, src_len, &v)) return 0;
    if (v.ttl == 0) return 0;   // TTL=0이면 더 이상 forward 불가
    if (v.total_len > dst_max) return 0;

    memcpy(dst, src, v.total_len);
    dst[2] = v.ttl - 1;   // TTL 필드만 변경
    return v.total_len;
}

void bitchat_id_from_sender(const uint8_t* sender_id, char* out_id) {
    if (!sender_id || !out_id) return;
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        out_id[i * 2]     = hex[(sender_id[i] >> 4) & 0xF];
        out_id[i * 2 + 1] = hex[sender_id[i] & 0xF];
    }
    out_id[16] = 0;
}

// 서명 검증용 메시지 계산
// BinaryProtocol.kt의 toBinaryDataForSigning() 포팅:
//   - signature 필드 제외 (HAS_SIGNATURE flag 클리어)
//   - TTL = 0 으로 설정
//   - 원본 버퍼의 wire bytes 그대로 사용 (압축/padding 보존)
//   - MessagePadding 적용 (PKCS#7, 블록 크기 256/512/1024/2048)
size_t bitchat_compute_signing_message(const uint8_t* pkt, size_t pkt_len,
                                         const bitchat_packet_view_t* v,
                                         uint8_t* out, size_t out_max) {
    if (!pkt || !v || !out) return 0;

    // 1. signature 제외한 바이트 길이
    size_t sig_offset = v->total_len;
    if (v->signature) {
        sig_offset -= BITCHAT_SIG_LEN;   // 64 bytes
    }
    if (sig_offset == 0 || sig_offset > pkt_len) {
        return 0;
    }

    // 2. 원본에서 signature 전까지 복사
    memcpy(out, pkt, sig_offset);

    // 3. TTL = 0 (byte offset 2)
    out[2] = 0;

    // 4. HAS_SIGNATURE flag 클리어 (byte offset 11, bit 1 = 0x02)
    out[11] &= ~0x02;

    // 5. MessagePadding 적용 (PKCS#7, bitchat 호환)
    //    blockSizes = [256, 512, 1024, 2048]
    //    totalSize = sig_offset + 16 (AES-GCM tag 오버헤드)
    //    가장 작은 블록 크기 선택 후 PKCS#7 패딩
    static const int blockSizes[] = {256, 512, 1024, 2048};
    int totalSize = (int)sig_offset + 16;
    int targetSize = (int)sig_offset;  // 기본값: 패딩 안 함
    for (int i = 0; i < 4; ++i) {
        if (totalSize <= blockSizes[i]) {
            targetSize = blockSizes[i];
            break;
        }
    }

    if (targetSize > (int)sig_offset && targetSize <= (int)out_max) {
        // PKCS#7 패딩: 남은 공간을 (targetSize - sig_offset) 값으로 채움
        int paddingNeeded = targetSize - (int)sig_offset;
        if (paddingNeeded > 0 && paddingNeeded <= 255) {
            memset(out + sig_offset, paddingNeeded, paddingNeeded);
            LOGD("signing message: %u bytes + %u bytes PKCS#7 padding -> %u total",
                 (unsigned)sig_offset, paddingNeeded, targetSize);
            return targetSize;
        }
    }

    // 패딩 불가 (너무 크거나 버퍼 부족) -> 패딩 없이 반환
    LOGD("signing message: %u bytes (no padding)", (unsigned)sig_offset);
    return sig_offset;
}

const char* bitchat_type_name(uint8_t type) {
    switch (type) {
        case BITCHAT_MSG_ANNOUNCE:         return "ANNOUNCE";
        case BITCHAT_MSG_MESSAGE:          return "MESSAGE";
        case BITCHAT_MSG_LEAVE:            return "LEAVE";
        case BITCHAT_MSG_NOISE_HANDSHAKE:  return "NOISE_HS";
        case BITCHAT_MSG_NOISE_ENCRYPTED:  return "NOISE_ENC";
        case BITCHAT_MSG_FRAGMENT:         return "FRAGMENT";
        case BITCHAT_MSG_REQUEST_SYNC:     return "REQ_SYNC";
        case BITCHAT_MSG_FILE_TRANSFER:    return "FILE_TX";
        case BITCHAT_MSG_VOICE_FRAME:      return "VOICE";
        default:                            return "UNKNOWN";
    }
}

void bitchat_flags_str(uint8_t flags, char* out) {
    if (!out) return;
    out[0] = 0;
    if (flags & BITCHAT_FLAG_HAS_RECIPIENT) strcat(out, "R");
    if (flags & BITCHAT_FLAG_HAS_SIGNATURE) strcat(out, "S");
    if (flags & BITCHAT_FLAG_IS_COMPRESSED) strcat(out, "C");
    if (flags & BITCHAT_FLAG_HAS_ROUTE)     strcat(out, "U");
    if (out[0] == 0) strcpy(out, "-");
}

bool bitchat_is_broadcast(const uint8_t* recipient_id) {
    if (!recipient_id) return true;   // recipient 없으면 broadcast
    for (int i = 0; i < 8; ++i) {
        if (recipient_id[i] != 0xFF) return false;
    }
    return true;
}
