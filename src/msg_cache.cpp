/* msg_cache.cpp - Store-and-forward 메시지 캐시 구현 */
#include "msg_cache.h"
#include "log.h"
#include <string.h>
#include <Arduino.h>

static msg_cache_entry_t g_cache[MSG_CACHE_SIZE];
static bool g_init = false;
static uint32_t g_next_slot = 0;
static uint32_t g_total_cached = 0;
static uint32_t g_total_flushed = 0;

bool msg_cache_init(void) {
    if (g_init) return true;
    memset(g_cache, 0, sizeof(g_cache));
    g_init = true;
    LOGI("msg_cache: initialized (size=%d, ttl=%lu min, ram=%u KB)",
         MSG_CACHE_SIZE, MSG_CACHE_TTL_MS / 60000,
         (unsigned)(sizeof(g_cache) / 1024));
    return true;
}

bool msg_cache_add(const uint8_t* data, size_t len) {
    if (!g_init) msg_cache_init();
    if (!data || len == 0 || len > sizeof(g_cache[0].data)) return false;
    uint32_t slot = g_next_slot;
    uint32_t oldest_ts = 0xFFFFFFFF;
    for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
        if (!g_cache[i].active) { slot = i; break; }
        if (g_cache[i].timestamp_ms < oldest_ts) { oldest_ts = g_cache[i].timestamp_ms; slot = i; }
    }
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
    for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
        if (!g_cache[i].active) continue;
        notify_fn(g_cache[i].data, g_cache[i].len);
        g_cache[i].active = false;
        flushed++;
    }
    if (flushed > 0) {
        g_total_flushed += flushed;
        LOGI("msg_cache: flushed %u messages (total_flushed=%u)", flushed, g_total_flushed);
    }
}

void msg_cache_cleanup(void) {
    if (!g_init) return;
    uint32_t now = millis();
    uint32_t removed = 0;
    for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
        if (!g_cache[i].active) continue;
        if (now - g_cache[i].timestamp_ms > MSG_CACHE_TTL_MS) { g_cache[i].active = false; removed++; }
    }
    if (removed > 0) LOGD("msg_cache: removed %u expired", removed);
}

void msg_cache_debug_dump(void) {
    LOGI("=== msg_cache: %u active, cached=%u flushed=%u ===",
         (unsigned)msg_cache_count(), g_total_cached, g_total_flushed);
    for (uint32_t i = 0; i < MSG_CACHE_SIZE; ++i) {
        if (!g_cache[i].active) continue;
        uint32_t age = (millis() - g_cache[i].timestamp_ms) / 1000;
        LOGI("  slot[%u] len=%u age=%lus", i, (unsigned)g_cache[i].len, age);
    }
}
