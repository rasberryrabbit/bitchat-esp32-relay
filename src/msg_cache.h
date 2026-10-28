/* msg_cache.h - Store-and-forward 메시지 캐시 (RAM, 12시간 TTL) */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MSG_CACHE_SIZE      32
#define MSG_CACHE_TTL_MS    (12UL * 60 * 60 * 1000)

typedef struct {
    bool      active;
    uint32_t  timestamp_ms;
    size_t    len;
    uint8_t   data[650];
} msg_cache_entry_t;

bool msg_cache_init(void);
bool msg_cache_add(const uint8_t* data, size_t len);
uint32_t msg_cache_count(void);
void msg_cache_flush(void (*notify_fn)(const uint8_t*, size_t));
void msg_cache_cleanup(void);
void msg_cache_debug_dump(void);
