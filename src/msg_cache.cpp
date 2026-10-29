/* msg_cache.cpp - Store-and-forward 메시지 캐시 구현 (RAM + SPIFFS overflow) */
#include "msg_cache.h"
#include "msg_store.h"   // SPIFFS overflow backend
#include "bitchat_protocol.h"   // bitchat_parse로 recipient_id 추출 (broadcast 필터)
#include "storage.h"   // storage_get_spiffs_cache_enabled() - SPIFFS write enable flag
#include "log.h"
#include <string.h>
#include <Arduino.h>

static msg_cache_entry_t g_cache[MSG_CACHE_SIZE];
static bool g_init = false;
static uint32_t g_next_slot = 0;
static uint32_t g_total_cached = 0;
static uint32_t g_total_flushed = 0;
static uint32_t g_overflow_to_store = 0;
static uint32_t g_overflow_dropped = 0;
static uint32_t g_overflow_disabled = 0;  // SPIFFS disabled flag에 의해 drop된 수

bool msg_cache_init(void) {
    if (g_init) return true;
    memset(g_cache, 0, sizeof(g_cache));

    // SPIFFS overflow store 초기화 (부팅 시 1회)
    msg_store_init();

    // SPIFFS에 저장된 이전 부팅의 overflow 메시지를 RAM으로 복원 시도
    // RAM에 빈 슬롯이 있으면 복원, 가득 차면 SPIFFS에 그대로 유지
    //   → 나중에 클라이언트 접속 시 msg_cache_flush가 SPIFFS 레코드도 함께 flush
    // bool 반환 콜백: true=성공(적재), false=실패(RAM 가득 참)
    size_t restored = msg_store_load_to_ram([](const uint8_t* data, size_t len) -> bool {
        if (len > sizeof(g_cache[0].data)) return false;  // too large, keep in SPIFFS
        for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
            if (!g_cache[i].active) {
                g_cache[i].active = true;
                g_cache[i].timestamp_ms = millis();
                g_cache[i].len = len;
                memcpy(g_cache[i].data, data, len);
                return true;  // 성공 → SPIFFS에서 inactive 마킹
            }
        }
        return false;  // RAM 가득 참 → SPIFFS에 유지
    });

    g_init = true;
    LOGI("msg_cache: initialized (size=%d, ttl=%lu min, ram=%u KB, restored_from_spiffs=%u)",
         MSG_CACHE_SIZE, MSG_CACHE_TTL_MS / 60000,
         (unsigned)(sizeof(g_cache) / 1024),
         (unsigned)restored);
    return true;
}

// RAM이 가득 찼을 때 가장 오래된 entry를 SPIFFS로 밀어냄
// 새 entry를 위한 공간 확보용
// broadcast 패킷은 SPIFFS에 안 쓰고 drop (flash wear 절약)
// SPIFFS 캐시 disabled (storage flag) 시에도 drop만 (SPIFFS write 안 함)
static bool overflow_oldest_to_store(void) {
    // 가장 오래된 entry 찾기
    uint32_t oldest_idx = 0;
    uint32_t oldest_ts = 0xFFFFFFFF;
    for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
        if (g_cache[i].active && g_cache[i].timestamp_ms < oldest_ts) {
            oldest_ts = g_cache[i].timestamp_ms;
            oldest_idx = i;
        }
    }
    if (oldest_ts == 0xFFFFFFFF) return false;  // 모두 비어 있음

    msg_cache_entry_t* old = &g_cache[oldest_idx];

    // SPIFFS 캐시 enabled 확인 (NVS 플래그, 기본 ON)
    // OFF이면 어떤 패킷이든 SPIFFS에 안 쓰고 drop만 (기존 데이터는 유지)
    if (!storage_get_spiffs_cache_enabled()) {
        g_overflow_disabled++;
        LOGD("msg_cache: overflow drop (spiffs_cache disabled, slot=%u, len=%u)",
             oldest_idx, (unsigned)old->len);
        // RAM에서 제거만 (SPIFFS write 없음)
        old->active = false;
        memset(old->data, 0, sizeof(old->data));
        old->len = 0;
        return true;
    }

    // broadcast 여부 확인 - broadcast는 SPIFFS에 안 쓰고 drop만
    bitchat_packet_view_t cv;
    bool is_broadcast = true;
    if (bitchat_parse(old->data, old->len, &cv)) {
        is_broadcast = (cv.recipient_id == nullptr) ||
                       bitchat_is_broadcast(cv.recipient_id);
    }

    if (is_broadcast) {
        // broadcast는 그냥 drop (SPIFFS write 안 함)
        g_overflow_dropped++;
        LOGD("msg_cache: overflow drop broadcast (slot=%u, len=%u)",
             oldest_idx, (unsigned)old->len);
    } else {
        // direct message → SPIFFS로 밀어냄
        if (msg_store_add(old->data, old->len)) {
            g_overflow_to_store++;
            LOGD("msg_cache: overflow → spiffs (slot=%u, len=%u, total_to_store=%u)",
                 oldest_idx, (unsigned)old->len, g_overflow_to_store);
        } else {
            // SPIFFS 쓰기 실패 (용량 초과 또는 패킷 too large) → drop
            g_overflow_dropped++;
        }
    }

    // RAM에서 제거
    old->active = false;
    memset(old->data, 0, sizeof(old->data));
    old->len = 0;
    return true;
}

bool msg_cache_add(const uint8_t* data, size_t len) {
    if (!g_init) msg_cache_init();
    if (!data || len == 0 || len > sizeof(g_cache[0].data)) return false;

    // 빈 슬롯 찾기
    int32_t empty_slot = -1;
    for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
        if (!g_cache[i].active) { empty_slot = (int32_t)i; break; }
    }

    // 빈 슬롯 없으면 가장 오래된 entry를 SPIFFS로 밀어냄
    if (empty_slot < 0) {
        if (!overflow_oldest_to_store()) {
            LOGE("msg_cache: overflow failed, cannot add");
            return false;
        }
        // 밀어낸 슬롯 재사용 - 다시 빈 슬롯 찾기
        for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
            if (!g_cache[i].active) { empty_slot = (int32_t)i; break; }
        }
        if (empty_slot < 0) {
            LOGE("msg_cache: still no empty slot after overflow");
            return false;
        }
    }

    uint32_t slot = (uint32_t)empty_slot;
    g_cache[slot].active = true;
    g_cache[slot].timestamp_ms = millis();
    g_cache[slot].len = len;
    memcpy(g_cache[slot].data, data, len);
    g_next_slot = (slot + 1) % MSG_CACHE_SIZE;
    g_total_cached++;
    LOGD("msg_cache: added (slot=%u, len=%u, total_cached=%u, active=%u)",
         slot, (unsigned)len, g_total_cached, (unsigned)msg_cache_count());
    return true;
}

uint32_t msg_cache_count(void) {
    if (!g_init) return 0;
    uint32_t c = 0;
    for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) if (g_cache[i].active) c++;
    return c;
}

void msg_cache_flush(void (*notify_fn)(const uint8_t*, size_t)) {
    if (!g_init || !notify_fn) return;
    uint32_t flushed = 0;
    // 1) RAM flush (기존 동작)
    for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
        if (!g_cache[i].active) continue;
        notify_fn(g_cache[i].data, g_cache[i].len);
        g_cache[i].active = false;
        flushed++;
    }
    // 2) SPIFFS flush - RAM에 있던 모든 것을 보낸 후 SPIFFS도 flush
    //    (client가 연결된 상태에서 모든 store-and-forward 메시지 전송)
    size_t spiffs_flushed = msg_store_flush_to_callback(notify_fn);
    flushed += (uint32_t)spiffs_flushed;

    if (flushed > 0) {
        g_total_flushed += flushed;
        LOGI("msg_cache: flushed %u messages (ram + spiffs, total_flushed=%u)",
             flushed, g_total_flushed);
    }
}

void msg_cache_cleanup(void) {
    if (!g_init) return;
    uint32_t now = millis();
    uint32_t removed = 0;
    for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
        if (!g_cache[i].active) continue;
        if (now - g_cache[i].timestamp_ms > MSG_CACHE_TTL_MS) {
            g_cache[i].active = false;
            removed++;
        }
    }
    if (removed > 0) LOGD("msg_cache: removed %u expired", removed);
}

void msg_cache_debug_dump(void) {
    LOGI("=== msg_cache: %u active, cached=%u flushed=%u ===",
         (unsigned)msg_cache_count(), g_total_cached, g_total_flushed);
    LOGI("  overflow: to_store=%u dropped=%u disabled=%u (spiffs_cache=%s)",
         g_overflow_to_store, g_overflow_dropped, g_overflow_disabled,
         storage_get_spiffs_cache_enabled() ? "ON" : "OFF");
    for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
        if (!g_cache[i].active) continue;
        uint32_t age = (millis() - g_cache[i].timestamp_ms) / 1000;
        LOGI("  ram slot[%u] len=%u age=%lus", i, (unsigned)g_cache[i].len, age);
    }
    // SPIFFS store 상태도 함께 출력
    msg_store_debug_dump();
}
