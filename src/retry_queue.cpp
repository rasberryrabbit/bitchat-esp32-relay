/*
 * retry_queue.cpp - ACK 재전송 로직 구현
 *
 * 한국어:
 *   16-슬롯 round-robin pending 큐로 forward한 패킷을 추적.
 *   passive ACK (같은 패킷 재송신 감지) 또는 explicit ACK (type=0x02)로
 *   entry 제거. 타임아웃 시 콜백으로 재송신, 최대 3회까지.
 */

#include "retry_queue.h"
#include "log.h"

#include <string.h>
#include <Arduino.h>

static retry_entry_t g_queue[BC_RETRY_QUEUE_SIZE];
static retry_stats_t g_stats;
static bool g_init_done = false;
static uint32_t g_next_slot = 0;   // round-robin 대상 슬롯

static void compute_hash(const uint8_t* sender_pk, uint64_t counter,
                          uint8_t out_hash[BC_RETRY_HASH_LEN]) {
    // sender_pk의 첫 8바이트 + counter 8바이트 (빅엔디언)
    memcpy(out_hash, sender_pk, 8);
    for (int i = 0; i < 8; ++i) {
        out_hash[8 + i] = (counter >> (8 * (7 - i))) & 0xFF;
    }
}

void retry_init(void) {
    if (g_init_done) return;
    memset(g_queue, 0, sizeof(g_queue));
    memset(&g_stats, 0, sizeof(g_stats));
    g_init_done = true;
    LOGI("retry_queue: initialized (size=%u, timeout=%ums, max_retries=%u)",
         BC_RETRY_QUEUE_SIZE, BC_RETRY_TIMEOUT_MS, BC_RETRY_MAX_RETRIES);
}

void retry_enqueue(const uint8_t* packet, size_t len) {
    if (!g_init_done) retry_init();
    if (!packet || len == 0 || len > BITCHAT_MAX_PKT_LEN) return;

    // 패킷 파싱해서 (sender_pk, counter) 추출
    bitchat_packet_view_t v;
    if (!bitchat_parse(packet, len, &v)) {
        LOGW("retry_enqueue: parse failed, not tracking");
        return;
    }

    // 해시 키 계산
    uint8_t hash[BC_RETRY_HASH_LEN];
    compute_hash(v.sender_id, v.timestamp, hash);

    // 이미 큐에 같은 해시가 있으면 갱신하지 않음 (중복 enqueue 방지)
    for (int i = 0; i < BC_RETRY_QUEUE_SIZE; ++i) {
        if (g_queue[i].active &&
            memcmp(g_queue[i].hash, hash, BC_RETRY_HASH_LEN) == 0) {
            LOGT("retry_enqueue: already tracked (slot %d)", i);
            return;
        }
    }

    // 빈 슬롯 찾기 (round-robin 시작)
    int slot = -1;
    for (int i = 0; i < BC_RETRY_QUEUE_SIZE; ++i) {
        int idx = (g_next_slot + i) % BC_RETRY_QUEUE_SIZE;
        if (!g_queue[idx].active) {
            slot = idx;
            break;
        }
    }

    if (slot < 0) {
        // 큐 꽉 참 - 가장 오래된 entry (first_sent_ms 가장 작은 것) 제거
        uint32_t oldest = 0xFFFFFFFF;
        for (int i = 0; i < BC_RETRY_QUEUE_SIZE; ++i) {
            if (g_queue[i].first_sent_ms < oldest) {
                oldest = g_queue[i].first_sent_ms;
                slot = i;
            }
        }
        LOGW("retry_enqueue: queue full, evicting oldest slot %d", slot);
        g_stats.total_dropped++;
        g_queue[slot].active = false;
    }

    // 새 entry 추가
    retry_entry_t* e = &g_queue[slot];
    memcpy(e->hash, hash, BC_RETRY_HASH_LEN);
    memcpy(e->packet, packet, len);
    e->packet_len = len;
    e->first_sent_ms = millis();
    e->last_sent_ms = millis();
    e->retry_count = 0;
    e->active = true;
    g_next_slot = (slot + 1) % BC_RETRY_QUEUE_SIZE;
    g_stats.total_enqueued++;
    g_stats.pending_count++;

    char id[BITCHAT_ID_STR_LEN];
    bitchat_id_from_pk(v.sender_id, id);
    LOGD("retry_enqueue: slot=%d id=%s ctr=%llu", slot, id,
         (unsigned long long)v.timestamp);
}

bool retry_check_passive_ack(const uint8_t* sender_pk, uint64_t counter) {
    if (!g_init_done || !sender_pk) return false;

    uint8_t hash[BC_RETRY_HASH_LEN];
    compute_hash(sender_pk, counter, hash);

    for (int i = 0; i < BC_RETRY_QUEUE_SIZE; ++i) {
        if (g_queue[i].active &&
            memcmp(g_queue[i].hash, hash, BC_RETRY_HASH_LEN) == 0) {
            // 패시브 ACK - 다른 peer가 이 패킷을 재전송했다는 것은
            // 우리 forward가 성공했다는 의미
            g_queue[i].active = false;
            g_stats.total_passive_acks++;
            if (g_stats.pending_count > 0) g_stats.pending_count--;
            LOGD("passive ACK slot=%d (after %ums)",
                 i, millis() - g_queue[i].first_sent_ms);
            return true;
        }
    }
    return false;
}

bool retry_process_explicit_ack(const uint8_t* ack_payload, size_t len) {
    if (!g_init_done || !ack_payload) return false;
    // ACK 패킷 payload: sender_pk(32B) || counter(8B) (최소 40B)
    if (len < 32 + 8) {
        LOGD("explicit ACK: payload too short %u", (unsigned)len);
        return false;
    }
    const uint8_t* sender_pk = ack_payload;
    uint64_t counter = 0;
    for (int i = 0; i < 8; ++i) {
        counter = (counter << 8) | ack_payload[32 + i];
    }
    // 같은 해시로 큐 검색
    if (retry_check_passive_ack(sender_pk, counter)) {
        g_stats.total_explicit_acks++;
        // (passive_ack에서 이미 total_passive_acks 증가시켰으니 보정)
        if (g_stats.total_passive_acks > 0) g_stats.total_passive_acks--;
        char id[BITCHAT_ID_STR_LEN];
        bitchat_id_from_pk(sender_pk, id);
        LOGI("explicit ACK from %s ctr=%llu", id, (unsigned long long)counter);
        return true;
    }
    return false;
}

void retry_check_timeouts(retry_resend_fn resend, void* user_data) {
    if (!g_init_done || !resend) return;

    uint32_t now = millis();
    for (int i = 0; i < BC_RETRY_QUEUE_SIZE; ++i) {
        retry_entry_t* e = &g_queue[i];
        if (!e->active) continue;

        // 타임아웃 체크
        if (now - e->last_sent_ms < BC_RETRY_TIMEOUT_MS) continue;

        // 재시도 한계 도달?
        if (e->retry_count >= BC_RETRY_MAX_RETRIES) {
            LOGW("retry: max retries (%u) reached for slot %d, dropping",
                 BC_RETRY_MAX_RETRIES, i);
            e->active = false;
            g_stats.total_dropped++;
            if (g_stats.pending_count > 0) g_stats.pending_count--;
            continue;
        }

        // 재전송
        e->retry_count++;
        e->last_sent_ms = now;
        g_stats.total_retransmits++;
        LOGI("retry: retransmit slot=%d attempt=%u/%u (after %ums)",
             i, e->retry_count, BC_RETRY_MAX_RETRIES,
             now - e->first_sent_ms);
        resend(e->packet, e->packet_len, user_data);
    }
}

void retry_debug_dump(void) {
    if (!g_init_done) return;
    uint32_t active = 0;
    for (int i = 0; i < BC_RETRY_QUEUE_SIZE; ++i) {
        if (g_queue[i].active) active++;
    }
    LOGI("=== retry_queue dump ===");
    LOGI("active=%u/%u total_enq=%u p_ack=%u e_ack=%u retrans=%u dropped=%u",
         active, BC_RETRY_QUEUE_SIZE,
         g_stats.total_enqueued, g_stats.total_passive_acks,
         g_stats.total_explicit_acks, g_stats.total_retransmits,
         g_stats.total_dropped);
    for (int i = 0; i < BC_RETRY_QUEUE_SIZE; ++i) {
        if (!g_queue[i].active) continue;
        uint32_t age = millis() - g_queue[i].first_sent_ms;
        LOGI("slot[%d] age=%ums retry=%u/%u len=%u",
             i, age, g_queue[i].retry_count, BC_RETRY_MAX_RETRIES,
             (unsigned)g_queue[i].packet_len);
    }
}

void retry_get_stats(retry_stats_t* out_stats) {
    if (!out_stats) return;
    *out_stats = g_stats;
}

void retry_clear(void) {
    memset(g_queue, 0, sizeof(g_queue));
    g_stats.pending_count = 0;
    g_next_slot = 0;
}
