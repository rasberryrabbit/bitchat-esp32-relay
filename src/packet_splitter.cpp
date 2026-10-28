/*
 * packet_splitter.cpp - 큰 패킷 분할/재조립 구현
 *
 * 한국어:
 *   16개 재조립 슬롯 관리. 각 슬롯은 BITCHAT_MAX_PKT_LEN 버퍼 + 비트맵 + 타임스탬프.
 *   sender_pk[0..7] XOR counter[0..7] = 8B hash로 슬롯 식별.
 *
 *   송신 측: 원본 패킷을 208B 청크로 분할 → magic 0xCF 헤더 붙여 송신
 *   수신 측: 청크 hash로 슬롯 찾기 → 비트맵에 청크 인덱스 기록
 *           → 모든 비트 1이면 원본 패킷 재조립 후 out_packet에 복사
 */

#include "packet_splitter.h"
#include "log.h"
#include "bitchat_protocol.h"

#include <string.h>
#include <Arduino.h>

// 재조립 슬롯 (16개 동시 진행)
typedef struct {
    bool     active;
    uint8_t  hash[8];              // 패킷 식별용 hash
    uint8_t  total_chunks;         // 총 청크 수
    uint8_t  received_mask;        // 비트맵 - 어느 청크 받았는지 (최대 16 청크)
    uint8_t  buffer[BITCHAT_MAX_PKT_LEN];
    size_t   total_len;             // 지금까지 받은 총 길이
    uint32_t first_recv_ms;        // 첫 청크 수신 시각 (타임아웃용)
    uint16_t chunk_payload_len;    // 각 청크의 payload 길이
} recon_slot_t;

static recon_slot_t g_slots[BC_MAX_CHUNKS_PER_PACKET];
static bool g_init_done = false;
static uint32_t g_total_split = 0;
static uint32_t g_total_reassembled = 0;
static uint32_t g_total_dropped = 0;

static void compute_hash(const uint8_t* sender_pk, uint64_t counter,
                          uint8_t out_hash[8]) {
    // sender_pk[0..7] XOR counter(big-endian 8 bytes)
    for (int i = 0; i < 8; ++i) {
        uint8_t c_byte = (uint8_t)((counter >> (8 * (7 - i))) & 0xFF);
        out_hash[i] = sender_pk[i] ^ c_byte;
    }
}

bool splitter_init(void) {
    if (g_init_done) return true;
    memset(g_slots, 0, sizeof(g_slots));
    g_init_done = true;
    LOGI("splitter: initialized (slots=%u, chunk_payload_max=%u, total=%uKB RAM)",
         BC_MAX_CHUNKS_PER_PACKET, BC_CHUNK_PAYLOAD_MAX,
         (unsigned)(sizeof(g_slots) / 1024));
    return true;
}

bool splitter_send_chunks(const uint8_t* packet, size_t packet_len,
                          chunk_tx_fn tx, void* user_data) {
    if (!g_init_done) splitter_init();
    if (!packet || !tx || packet_len == 0) return false;
    if (packet_len > BITCHAT_MAX_PKT_LEN) {
        LOGW("splitter: packet too large %u > %u", (unsigned)packet_len,
             BITCHAT_MAX_PKT_LEN);
        return false;
    }

    // 패킷 파싱 - sender_pk + counter 추출 (hash 계산용)
    bitchat_packet_view_t v;
    if (!bitchat_parse(packet, packet_len, &v)) {
        LOGW("splitter: failed to parse bitchat packet");
        return false;
    }
    uint8_t hash[8];
    compute_hash(v.sender_pk, v.counter, hash);

    // 청크 수 계산
    size_t num_chunks = (packet_len + BC_CHUNK_PAYLOAD_MAX - 1) / BC_CHUNK_PAYLOAD_MAX;
    if (num_chunks > BC_MAX_CHUNKS_PER_PACKET) {
        LOGW("splitter: too many chunks %u > %u", (unsigned)num_chunks,
             BC_MAX_CHUNKS_PER_PACKET);
        return false;
    }

    // 각 청크 송신
    for (size_t i = 0; i < num_chunks; ++i) {
        uint8_t chunk[BC_CHUNK_HEADER_LEN + BC_CHUNK_PAYLOAD_MAX];
        size_t offset = i * BC_CHUNK_PAYLOAD_MAX;
        size_t this_payload_len = (i == num_chunks - 1)
            ? (packet_len - offset)
            : BC_CHUNK_PAYLOAD_MAX;

        // 청크 헤더 작성
        chunk[0] = BC_CHUNK_MAGIC;
        chunk[1] = BC_CHUNK_VERSION;
        chunk[2] = (uint8_t)num_chunks;
        chunk[3] = (uint8_t)i;
        memcpy(chunk + 4, hash, 8);
        chunk[12] = (uint8_t)((this_payload_len >> 8) & 0xFF);
        chunk[13] = (uint8_t)(this_payload_len & 0xFF);
        // 페이로드 복사
        memcpy(chunk + BC_CHUNK_HEADER_LEN, packet + offset, this_payload_len);

        size_t chunk_len = BC_CHUNK_HEADER_LEN + this_payload_len;
        if (!tx(chunk, chunk_len, user_data)) {
            LOGW("splitter: chunk %u/%u TX failed", (unsigned)i,
                 (unsigned)num_chunks);
            return false;
        }
        LOGD("splitter: TX chunk %u/%u (%u bytes payload)",
             (unsigned)i, (unsigned)num_chunks, (unsigned)this_payload_len);
    }
    g_total_split++;
    LOGI("splitter: sent %u chunks for packet (total_len=%u, hash=0x%02x%02x%02x%02x)",
         (unsigned)num_chunks, (unsigned)packet_len,
         hash[0], hash[1], hash[2], hash[3]);
    return true;
}

// 빈 슬롯 찾기
static recon_slot_t* find_or_alloc_slot(const uint8_t hash[8],
                                         uint8_t total_chunks) {
    // 같은 hash 찾기
    for (int i = 0; i < BC_MAX_CHUNKS_PER_PACKET; ++i) {
        if (g_slots[i].active && memcmp(g_slots[i].hash, hash, 8) == 0) {
            return &g_slots[i];
        }
    }
    // 빈 슬롯 찾기
    for (int i = 0; i < BC_MAX_CHUNKS_PER_PACKET; ++i) {
        if (!g_slots[i].active) {
            memset(&g_slots[i], 0, sizeof(g_slots[i]));
            memcpy(g_slots[i].hash, hash, 8);
            g_slots[i].total_chunks = total_chunks;
            g_slots[i].active = true;
            g_slots[i].first_recv_ms = millis();
            return &g_slots[i];
        }
    }
    // 꽉 참 - 가장 오래된 슬롯 제거
    uint32_t oldest = 0xFFFFFFFF;
    int oldest_idx = 0;
    for (int i = 0; i < BC_MAX_CHUNKS_PER_PACKET; ++i) {
        if (g_slots[i].first_recv_ms < oldest) {
            oldest = g_slots[i].first_recv_ms;
            oldest_idx = i;
        }
    }
    LOGW("splitter: slots full, evicting oldest (idx=%d)", oldest_idx);
    g_total_dropped++;
    memset(&g_slots[oldest_idx], 0, sizeof(g_slots[oldest_idx]));
    memcpy(g_slots[oldest_idx].hash, hash, 8);
    g_slots[oldest_idx].total_chunks = total_chunks;
    g_slots[oldest_idx].active = true;
    g_slots[oldest_idx].first_recv_ms = millis();
    return &g_slots[oldest_idx];
}

bool splitter_receive_chunk(const uint8_t* chunk, size_t chunk_len,
                             uint8_t* out_packet, size_t* out_len) {
    if (!g_init_done) splitter_init();
    if (!chunk || chunk_len < BC_CHUNK_HEADER_LEN) return false;
    if (chunk[0] != BC_CHUNK_MAGIC) return false;
    if (chunk[1] != BC_CHUNK_VERSION) return false;

    uint8_t num_chunks = chunk[2];
    uint8_t chunk_idx  = chunk[3];
    const uint8_t* hash = chunk + 4;
    uint16_t payload_len = (uint16_t)((chunk[12] << 8) | chunk[13]);

    if (num_chunks > BC_MAX_CHUNKS_PER_PACKET) {
        LOGW("splitter: too many chunks %u", num_chunks);
        return false;
    }
    if (chunk_idx >= num_chunks) {
        LOGW("splitter: chunk_idx %u >= num_chunks %u", chunk_idx, num_chunks);
        return false;
    }
    if (chunk_len != BC_CHUNK_HEADER_LEN + payload_len) {
        LOGW("splitter: chunk_len mismatch %u != %u + %u",
             (unsigned)chunk_len, BC_CHUNK_HEADER_LEN, payload_len);
        return false;
    }

    recon_slot_t* slot = find_or_alloc_slot(hash, num_chunks);

    // 이미 받은 청크인지 확인
    uint8_t mask_bit = (uint8_t)(1 << chunk_idx);
    if (slot->received_mask & mask_bit) {
        LOGD("splitter: dup chunk %u/%u, ignoring", chunk_idx, num_chunks);
        return false;
    }

    // 청크 페이로드를 버퍼에 저장
    size_t offset = chunk_idx * BC_CHUNK_PAYLOAD_MAX;
    if (offset + payload_len > sizeof(slot->buffer)) {
        LOGW("splitter: chunk offset overflow");
        return false;
    }
    memcpy(slot->buffer + offset, chunk + BC_CHUNK_HEADER_LEN, payload_len);
    slot->received_mask |= mask_bit;
    slot->chunk_payload_len = BC_CHUNK_PAYLOAD_MAX;

    // 모든 청크 받았는지 확인 (비트맵)
    uint8_t all_received = (uint8_t)((1 << num_chunks) - 1);
    if ((slot->received_mask & all_received) == all_received) {
        // 원본 패킷 길이 계산
        size_t original_len = (num_chunks - 1) * BC_CHUNK_PAYLOAD_MAX;
        // 마지막 청크 길이 추출 - 마지막 청크의 payload_len 사용
        // (이미 위에서 offset 계산에 사용, 마지막 청크 길이 = payload_len)
        original_len += payload_len;

        if (out_packet && out_len) {
            if (original_len > BITCHAT_MAX_PKT_LEN) {
                LOGW("splitter: reconstructed too large %u", (unsigned)original_len);
                slot->active = false;
                g_total_dropped++;
                return false;
            }
            memcpy(out_packet, slot->buffer, original_len);
            *out_len = original_len;
        }
        slot->active = false;
        g_total_reassembled++;
        LOGI("splitter: ✓ reassembled %u chunks → %u bytes (hash=0x%02x%02x%02x%02x)",
             num_chunks, (unsigned)original_len,
             hash[0], hash[1], hash[2], hash[3]);
        return true;
    }
    LOGD("splitter: chunk %u/%u stored, mask=0x%02x (need 0x%02x)",
         chunk_idx, num_chunks, slot->received_mask, all_received);
    return false;
}

bool splitter_is_chunk(const uint8_t* data, size_t len) {
    if (!data || len < 4) return false;
    return data[0] == BC_CHUNK_MAGIC && data[1] == BC_CHUNK_VERSION;
}

void splitter_cleanup_timeouts(void) {
    if (!g_init_done) return;
    uint32_t now = millis();
    for (int i = 0; i < BC_MAX_CHUNKS_PER_PACKET; ++i) {
        if (g_slots[i].active && (now - g_slots[i].first_recv_ms) > BC_CHUNK_TIMEOUT_MS) {
            LOGW("splitter: slot %d timeout (hash=0x%02x%02x%02x%02x)",
                 i, g_slots[i].hash[0], g_slots[i].hash[1],
                 g_slots[i].hash[2], g_slots[i].hash[3]);
            g_slots[i].active = false;
            g_total_dropped++;
        }
    }
}

void splitter_debug_dump(void) {
    if (!g_init_done) return;
    uint32_t active = 0;
    for (int i = 0; i < BC_MAX_CHUNKS_PER_PACKET; ++i) {
        if (g_slots[i].active) active++;
    }
    LOGI("=== splitter dump ===");
    LOGI("active: %u/%u, total_split=%u reassembled=%u dropped=%u",
         active, BC_MAX_CHUNKS_PER_PACKET,
         g_total_split, g_total_reassembled, g_total_dropped);
    for (int i = 0; i < BC_MAX_CHUNKS_PER_PACKET; ++i) {
        if (!g_slots[i].active) continue;
        uint8_t needed = (uint8_t)((1 << g_slots[i].total_chunks) - 1);
        uint8_t got = g_slots[i].received_mask;
        uint8_t missing = needed & ~got;
        uint32_t age = millis() - g_slots[i].first_recv_ms;
        LOGI("slot[%d] hash=0x%02x%02x%02x%02x chunks=%u/%u mask=0x%02x age=%ums",
             i, g_slots[i].hash[0], g_slots[i].hash[1], g_slots[i].hash[2],
             g_slots[i].hash[3],
             __builtin_popcount(got), g_slots[i].total_chunks,
             got, age);
        (void)missing;
    }
}
