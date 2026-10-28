/*
 * bitchat_protocol.cpp - bitchat packet encode/decode implementation
 *
 * 한국어: 패킷 직렬화/역직렬화를 구현합니다. 모든 함수는 heap 할당 없이
 *        caller가 제공한 버퍼에만 동작합니다.
 */

#include "bitchat_protocol.h"
#include "log.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

// 빅엔디언 read/write 헬퍼
static inline uint64_t rd_be_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

static inline uint16_t rd_be_u16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void wr_be_u64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

static inline void wr_be_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

bool bitchat_parse(const uint8_t* buf, size_t len, bitchat_packet_view_t* view) {
    if (!buf || !view) return false;
    if (len < BITCHAT_MIN_PKT_LEN) {
        LOGD("parse fail: too short len=%u min=%u",
             (unsigned)len, (unsigned)BITCHAT_MIN_PKT_LEN);
        return false;
    }
    if (len > BITCHAT_MAX_PKT_LEN) {
        LOGD("parse fail: too long len=%u max=%u",
             (unsigned)len, (unsigned)BITCHAT_MAX_PKT_LEN);
        return false;
    }
    if (buf[0] != BITCHAT_MAGIC) {
        LOGT("parse fail: bad magic 0x%02x", buf[0]);
        return false;
    }
    uint8_t ver    = buf[1];
    uint8_t type   = buf[2];
    uint8_t ttl    = buf[3];
    if (ver != BITCHAT_VERSION) {
        LOGD("parse fail: unsupported ver=%u", ver);
        return false;
    }
    if (ttl > BITCHAT_MAX_TTL) {
        LOGD("parse fail: ttl=%u exceeds max=%u", ttl, BITCHAT_MAX_TTL);
        return false;
    }
    const uint8_t* pk        = buf + 4;
    const uint8_t* counter_p = buf + 4 + BITCHAT_PUBKEY_LEN;
    const uint8_t* len_p     = counter_p + 8;
    uint16_t payload_len     = rd_be_u16(len_p);
    if (payload_len > BITCHAT_MAX_PAYLOAD_LEN) {
        LOGD("parse fail: payload_len=%u exceeds max", payload_len);
        return false;
    }
    // 헤더(고정) + payload_len + sig(64) == 총 길이 여야 함
    size_t expected = BITCHAT_HEADER_LEN + (size_t)payload_len + BITCHAT_SIG_LEN;
    if (len != expected) {
        LOGD("parse fail: length mismatch len=%u expected=%u plen=%u",
             (unsigned)len, (unsigned)expected, payload_len);
        return false;
    }
    const uint8_t* payload = len_p + 2;
    const uint8_t* sig      = payload + payload_len;
    view->magic        = buf[0];
    view->version      = ver;
    view->type         = type;
    view->ttl          = ttl;
    view->sender_pk    = pk;
    view->counter      = rd_be_u64(counter_p);
    view->payload_len  = payload_len;
    view->payload      = payload;
    view->signature    = sig;
    return true;
}

size_t bitchat_total_len(const bitchat_packet_view_t* v) {
    if (!v) return 0;
    return BITCHAT_HEADER_LEN + v->payload_len + BITCHAT_SIG_LEN;
}

void bitchat_id_from_pk(const uint8_t* pk, char* out_id) {
    if (!pk || !out_id) return;
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
        out_id[i * 2]     = hex[(pk[i] >> 4) & 0xF];
        out_id[i * 2 + 1] = hex[pk[i] & 0xF];
    }
    out_id[12] = 0;
}

void bitchat_id_from_mac(const uint8_t* mac, char* out_id) {
    if (!mac || !out_id) return;
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
        out_id[i * 2]     = hex[(mac[i] >> 4) & 0xF];
        out_id[i * 2 + 1] = hex[mac[i] & 0xF];
    }
    out_id[12] = 0;
}

void bitchat_packet_debug(const bitchat_packet_view_t* v) {
    if (!v) return;
    char id[BITCHAT_ID_STR_LEN];
    bitchat_id_from_pk(v->sender_pk, id);
    const char* type_str = "?";
    switch (v->type) {
        case BC_TYPE_CHAT:         type_str = "CHAT"; break;
        case BC_TYPE_ACK:          type_str = "ACK"; break;
        case BC_TYPE_PEER_INFO:    type_str = "PEER_INFO"; break;
        case BC_TYPE_PEER_REQUEST: type_str = "PEER_REQUEST"; break;
        case BC_TYPE_LEAVE:        type_str = "LEAVE"; break;
        case BC_TYPE_KEY_EXCHANGE: type_str = "KEY_EXCHANGE"; break;
        case BC_TYPE_PING:         type_str = "PING"; break;
        default:                   type_str = "UNKNOWN"; break;
    }
    LOGD("pkt v=%u type=0x%02x(%s) ttl=%u sender=%s ctr=%llu plen=%u",
         v->version, v->type, type_str, v->ttl, id,
         (unsigned long long)v->counter, v->payload_len);
}

void bitchat_signing_message(const bitchat_packet_view_t* v,
                             uint8_t* out_buf, size_t* out_len) {
    if (!v || !out_buf || !out_len) {
        if (out_len) *out_len = 0;
        return;
    }
    size_t off = 0;
    out_buf[off++] = v->magic;
    out_buf[off++] = v->version;
    out_buf[off++] = v->type;
    out_buf[off++] = v->ttl;
    memcpy(out_buf + off, v->sender_pk, BITCHAT_PUBKEY_LEN);
    off += BITCHAT_PUBKEY_LEN;
    wr_be_u64(out_buf + off, v->counter);
    off += 8;
    wr_be_u16(out_buf + off, v->payload_len);
    off += 2;
    if (v->payload_len) {
        memcpy(out_buf + off, v->payload, v->payload_len);
        off += v->payload_len;
    }
    *out_len = off;
}

size_t bitchat_build_header(bitchat_packet_builder_t* b,
                            uint8_t type, uint8_t ttl,
                            const uint8_t* sender_pk, uint32_t counter,
                            const uint8_t* payload, uint16_t payload_len) {
    if (!b || !sender_pk) return 0;
    if (payload_len > 0 && !payload) return 0;
    if (payload_len > BITCHAT_MAX_PAYLOAD_LEN) return 0;
    if (ttl > BITCHAT_MAX_TTL) return 0;

    size_t off = 0;
    b->buffer[off++] = BITCHAT_MAGIC;
    b->buffer[off++] = BITCHAT_VERSION;
    b->buffer[off++] = type;
    b->buffer[off++] = ttl;
    memcpy(b->buffer + off, sender_pk, BITCHAT_PUBKEY_LEN);
    off += BITCHAT_PUBKEY_LEN;
    wr_be_u64(b->buffer + off, counter);
    off += 8;
    wr_be_u16(b->buffer + off, payload_len);
    off += 2;
    if (payload_len) {
        memcpy(b->buffer + off, payload, payload_len);
        off += payload_len;
    }
    b->used = off;
    return off;
}

size_t bitchat_builder_reserve_sig(bitchat_packet_builder_t* b) {
    if (!b) return 0;
    if (b->used + BITCHAT_SIG_LEN > sizeof(b->buffer)) return 0;
    size_t sig_off = b->used;
    // 서명 자리는 0으로 채워둠 - crypto 모듈이 나중에 덮어씀
    memset(b->buffer + sig_off, 0, BITCHAT_SIG_LEN);
    b->used += BITCHAT_SIG_LEN;
    return sig_off;
}

size_t bitchat_decrement_ttl(const uint8_t* src, size_t src_len,
                             uint8_t* dst, size_t dst_max) {
    bitchat_packet_view_t v;
    if (!bitchat_parse(src, src_len, &v)) return 0;
    if (v.ttl == 0) {
        // 이미 TTL=0, 더 이상 중계 불가
        return 0;
    }
    size_t total = bitchat_total_len(&v);
    if (total > dst_max) return 0;
    memcpy(dst, src, total);
    dst[3] = v.ttl - 1;
    // 서명은 재서명 필요 (중계 노드가 아닌 송신자가 서명하므로,
    //                  TTL만 바꾸면 기존 서명이 무효가 됨)
    // -> 실제 중계 시나리오에서는 bitchat이 mesh-friendly하게
    //    TTL만 바꾸고 서명은 송신자 PK 기준 유지하는 정책을 쓰거나,
    //    아니면 relay node가 자체 서명을 다시 붙임.
    //    본 펌웨어에서는 원본 송신자 서명 유지 + TTL만 감소 정책 사용
    //    (bitchat 메시 멜버들은 sender_pk로 원본 출처를 신뢰할 수 있으므로).
    return total;
}
