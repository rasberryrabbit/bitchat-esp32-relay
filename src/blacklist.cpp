/*
 * blacklist.cpp - 블랙리스트 필터링 구현 (SPIFFS 백엔드)
 *
 * 한국어:
 *   pk(32B) 256개 + BLE(6B) 128개를 SPIFFS 파일로 영속화.
 *   - PK 파일: /blacklist_pk.bin   256 × 33B = 8,448B
 *   - BLE 파일: /blacklist_ble.bin 128 × 7B  =   896B
 *
 *   구조:
 *     pk_entry_t  = { uint8_t active; uint8_t pk[32]; }    // 33B
 *     ble_entry_t = { uint8_t active; uint8_t addr[6]; }   // 7B
 *
 *   동작:
 *     - init(): SPIFFS에서 두 파일 로드 (없으면 빈 파일 생성)
 *     - add/remove: RAM 배열 갱신 후 통째로 SPIFFS에 저장
 *     - clear: RAM 초기화 + 파일 삭제
 *
 *   SPIFFS vs NVS 장점:
 *     - 용량 8배 확장 (32→256 pk, 16→128 ble)
 *     - 파일 단위 저장 → blob 4KB 한계 없음
 *     - NVS는 자체 키/피어/카운터 전용으로 분리
 */

#include "blacklist.h"
#include "log.h"
#include "bitchat_protocol.h"   // BITCHAT_PUBKEY_LEN

#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <string.h>
#include <stdio.h>

// 직렬화 포맷: [active:1B][pk:32B] × N
typedef struct {
    uint8_t active;
    uint8_t pk[32];
} pk_entry_t;

typedef struct {
    uint8_t active;
    uint8_t addr[6];
} ble_entry_t;

static pk_entry_t  g_pk_entries[BC_BL_MAX_PK_ENTRIES];
static ble_entry_t g_ble_entries[BC_BL_MAX_BLE_ENTRIES];
static bool g_enabled = true;
static bool g_init_done = false;
static size_t g_pk_count = 0;
static size_t g_ble_count = 0;

// SPIFFS에 파일 저장 (통째로 write)
// RAM 배열 → 파일 동기화. 호출 시마다 전체 파일 재작성.
static bool persist_pk_to_spiffs(void) {
    File f = SPIFFS.open(BC_BL_PK_FILE, "w");
    if (!f) {
        LOGE("blacklist: failed to open %s for write", BC_BL_PK_FILE);
        return false;
    }
    size_t written = f.write((const uint8_t*)g_pk_entries, sizeof(g_pk_entries));
    f.close();
    if (written != sizeof(g_pk_entries)) {
        LOGE("blacklist: pk write short: %u/%u",
             (unsigned)written, (unsigned)sizeof(g_pk_entries));
        return false;
    }
    return true;
}

static bool persist_ble_to_spiffs(void) {
    File f = SPIFFS.open(BC_BL_BLE_FILE, "w");
    if (!f) {
        LOGE("blacklist: failed to open %s for write", BC_BL_BLE_FILE);
        return false;
    }
    size_t written = f.write((const uint8_t*)g_ble_entries, sizeof(g_ble_entries));
    f.close();
    if (written != sizeof(g_ble_entries)) {
        LOGE("blacklist: ble write short: %u/%u",
             (unsigned)written, (unsigned)sizeof(g_ble_entries));
        return false;
    }
    return true;
}

// SPIFFS에서 파일 로드
static bool load_pk_from_spiffs(void) {
    if (!SPIFFS.exists(BC_BL_PK_FILE)) {
        LOGI("blacklist: %s not found, starting empty", BC_BL_PK_FILE);
        memset(g_pk_entries, 0, sizeof(g_pk_entries));
        return false;
    }
    File f = SPIFFS.open(BC_BL_PK_FILE, "r");
    if (!f) {
        LOGE("blacklist: failed to open %s for read", BC_BL_PK_FILE);
        memset(g_pk_entries, 0, sizeof(g_pk_entries));
        return false;
    }
    size_t got = f.readBytes((char*)g_pk_entries, sizeof(g_pk_entries));
    f.close();
    if (got != sizeof(g_pk_entries)) {
        LOGW("blacklist: pk file truncated (%u/%u), padding zeros",
             (unsigned)got, (unsigned)sizeof(g_pk_entries));
        // 부분 로드 시 나머지 0으로 채움
        if (got < sizeof(g_pk_entries)) {
            memset((uint8_t*)g_pk_entries + got, 0, sizeof(g_pk_entries) - got);
        }
    }
    return true;
}

static bool load_ble_from_spiffs(void) {
    if (!SPIFFS.exists(BC_BL_BLE_FILE)) {
        LOGI("blacklist: %s not found, starting empty", BC_BL_BLE_FILE);
        memset(g_ble_entries, 0, sizeof(g_ble_entries));
        return false;
    }
    File f = SPIFFS.open(BC_BL_BLE_FILE, "r");
    if (!f) {
        LOGE("blacklist: failed to open %s for read", BC_BL_BLE_FILE);
        memset(g_ble_entries, 0, sizeof(g_ble_entries));
        return false;
    }
    size_t got = f.readBytes((char*)g_ble_entries, sizeof(g_ble_entries));
    f.close();
    if (got != sizeof(g_ble_entries)) {
        LOGW("blacklist: ble file truncated (%u/%u), padding zeros",
             (unsigned)got, (unsigned)sizeof(g_ble_entries));
        if (got < sizeof(g_ble_entries)) {
            memset((uint8_t*)g_ble_entries + got, 0, sizeof(g_ble_entries) - got);
        }
    }
    return true;
}

static void recount_entries(void) {
    g_pk_count = 0;
    for (int i = 0; i < BC_BL_MAX_PK_ENTRIES; ++i) {
        if (g_pk_entries[i].active) g_pk_count++;
    }
    g_ble_count = 0;
    for (int i = 0; i < BC_BL_MAX_BLE_ENTRIES; ++i) {
        if (g_ble_entries[i].active) g_ble_count++;
    }
}

bool blacklist_init(void) {
    if (g_init_done) return true;

    // SPIFFS 마운트 확인 (main.cpp에서 SPIFFS.begin(true) 선행 필요)
    if (!SPIFFS.begin(true)) {
        LOGE("blacklist: SPIFFS mount failed, blacklist disabled");
        return false;
    }

    memset(g_pk_entries, 0, sizeof(g_pk_entries));
    memset(g_ble_entries, 0, sizeof(g_ble_entries));

    load_pk_from_spiffs();
    load_ble_from_spiffs();
    recount_entries();

    g_init_done = true;
    g_enabled = true;
    LOGI("blacklist: initialized from SPIFFS (enabled=%d, pk=%u/%u, ble=%u/%u)",
         g_enabled,
         (unsigned)g_pk_count,  BC_BL_MAX_PK_ENTRIES,
         (unsigned)g_ble_count, BC_BL_MAX_BLE_ENTRIES);
    LOGI("blacklist: SPIFFS usage: pk=%uB + ble=%uB (of %uB partition)",
         (unsigned)sizeof(g_pk_entries),
         (unsigned)sizeof(g_ble_entries),
         (unsigned)128 * 1024);
    return true;
}

void blacklist_set_enabled(bool enabled) {
    g_enabled = enabled;
    LOGI("blacklist: %s", g_enabled ? "ENABLED" : "DISABLED");
}

bool blacklist_is_enabled(void) {
    return g_enabled;
}

// ----- Ed25519 공개키 -----

bool blacklist_add_pk(const uint8_t* pk) {
    if (!g_init_done || !pk) return false;
    if (blacklist_is_pk_blocked(pk)) return false;  // already

    // 빈 슬롯 찾기
    for (int i = 0; i < BC_BL_MAX_PK_ENTRIES; ++i) {
        if (!g_pk_entries[i].active) {
            g_pk_entries[i].active = 1;
            memcpy(g_pk_entries[i].pk, pk, 32);
            g_pk_count++;
            persist_pk_to_spiffs();
            char id[BITCHAT_ID_STR_LEN];
            bitchat_id_from_pk(pk, id);
            LOGI("blacklist: added pk id=%s (slot %d, total=%u/%u)",
                 id, i, (unsigned)g_pk_count, BC_BL_MAX_PK_ENTRIES);
            return true;
        }
    }
    LOGW("blacklist: pk full (%u/%u), cannot add",
         (unsigned)g_pk_count, BC_BL_MAX_PK_ENTRIES);
    return false;
}

bool blacklist_remove_pk(const uint8_t* pk) {
    if (!g_init_done || !pk) return false;
    for (int i = 0; i < BC_BL_MAX_PK_ENTRIES; ++i) {
        if (g_pk_entries[i].active &&
            memcmp(g_pk_entries[i].pk, pk, 32) == 0) {
            g_pk_entries[i].active = 0;
            memset(g_pk_entries[i].pk, 0, 32);
            g_pk_count--;
            persist_pk_to_spiffs();
            char id[BITCHAT_ID_STR_LEN];
            bitchat_id_from_pk(pk, id);
            LOGI("blacklist: removed pk id=%s (total=%u/%u)",
                 id, (unsigned)g_pk_count, BC_BL_MAX_PK_ENTRIES);
            return true;
        }
    }
    return false;
}

bool blacklist_is_pk_blocked(const uint8_t* pk) {
    if (!g_init_done || !g_enabled || !pk) return false;
    // 256개라 linear scan도 충분 (~8μs @ 240MHz)
    // 필요시 정렬 + binary search로 최적화 가능
    for (int i = 0; i < BC_BL_MAX_PK_ENTRIES; ++i) {
        if (g_pk_entries[i].active &&
            memcmp(g_pk_entries[i].pk, pk, 32) == 0) {
            return true;
        }
    }
    return false;
}

// ----- BLE 주소 -----

bool blacklist_add_ble(const uint8_t* addr) {
    if (!g_init_done || !addr) return false;
    if (blacklist_is_ble_blocked(addr)) return false;

    for (int i = 0; i < BC_BL_MAX_BLE_ENTRIES; ++i) {
        if (!g_ble_entries[i].active) {
            g_ble_entries[i].active = 1;
            memcpy(g_ble_entries[i].addr, addr, 6);
            g_ble_count++;
            persist_ble_to_spiffs();
            char hex[13];
            static const char* h = "0123456789abcdef";
            for (int j = 0; j < 6; ++j) {
                hex[j * 2]     = h[(addr[j] >> 4) & 0xF];
                hex[j * 2 + 1] = h[addr[j] & 0xF];
            }
            hex[12] = 0;
            LOGI("blacklist: added ble %s (slot %d, total=%u/%u)",
                 hex, i, (unsigned)g_ble_count, BC_BL_MAX_BLE_ENTRIES);
            return true;
        }
    }
    LOGW("blacklist: ble full (%u/%u), cannot add",
         (unsigned)g_ble_count, BC_BL_MAX_BLE_ENTRIES);
    return false;
}

bool blacklist_remove_ble(const uint8_t* addr) {
    if (!g_init_done || !addr) return false;
    for (int i = 0; i < BC_BL_MAX_BLE_ENTRIES; ++i) {
        if (g_ble_entries[i].active &&
            memcmp(g_ble_entries[i].addr, addr, 6) == 0) {
            g_ble_entries[i].active = 0;
            memset(g_ble_entries[i].addr, 0, 6);
            g_ble_count--;
            persist_ble_to_spiffs();
            return true;
        }
    }
    return false;
}

bool blacklist_is_ble_blocked(const uint8_t* addr) {
    if (!g_init_done || !g_enabled || !addr) return false;
    for (int i = 0; i < BC_BL_MAX_BLE_ENTRIES; ++i) {
        if (g_ble_entries[i].active &&
            memcmp(g_ble_entries[i].addr, addr, 6) == 0) {
            return true;
        }
    }
    return false;
}

// ----- 유틸리티 -----

void blacklist_clear_all(void) {
    memset(g_pk_entries, 0, sizeof(g_pk_entries));
    memset(g_ble_entries, 0, sizeof(g_ble_entries));
    g_pk_count = 0;
    g_ble_count = 0;
    // SPIFFS에서도 파일 삭제
    SPIFFS.remove(BC_BL_PK_FILE);
    SPIFFS.remove(BC_BL_BLE_FILE);
    LOGI("blacklist: cleared all (SPIFFS files removed)");
}

size_t blacklist_get_pk_count(void)  { return g_pk_count; }
size_t blacklist_get_ble_count(void) { return g_ble_count; }

void blacklist_debug_dump(void) {
    LOGI("=== blacklist dump (enabled=%d, SPIFFS backend) ===", g_enabled);
    LOGI("pk entries: %u/%u (file: %s, %uB)",
         (unsigned)g_pk_count, BC_BL_MAX_PK_ENTRIES,
         BC_BL_PK_FILE, (unsigned)sizeof(g_pk_entries));
    for (int i = 0; i < BC_BL_MAX_PK_ENTRIES; ++i) {
        if (!g_pk_entries[i].active) continue;
        char id[BITCHAT_ID_STR_LEN];
        bitchat_id_from_pk(g_pk_entries[i].pk, id);
        LOGI("  pk[%03d] id=%s", i, id);
    }
    LOGI("ble entries: %u/%u (file: /blacklist_ble.bin, %uB)",
         (unsigned)g_ble_count, BC_BL_MAX_BLE_ENTRIES,
         (unsigned)sizeof(g_ble_entries));
    for (int i = 0; i < BC_BL_MAX_BLE_ENTRIES; ++i) {
        if (!g_ble_entries[i].active) continue;
        char hex[13];
        static const char* h = "0123456789abcdef";
        for (int j = 0; j < 6; ++j) {
            hex[j * 2]     = h[(g_ble_entries[i].addr[j] >> 4) & 0xF];
            hex[j * 2 + 1] = h[g_ble_entries[i].addr[j] & 0xF];
        }
        hex[12] = 0;
        LOGI("  ble[%03d] %s", i, hex);
    }
    // SPIFFS 전체 사용량 표시 (arduino-esp32 v2.x: totalBytes()/usedBytes())
    size_t total = SPIFFS.totalBytes();
    size_t used  = SPIFFS.usedBytes();
    LOGI("spiffs: used=%uB / total=%uB (%.1f%%)",
         (unsigned)used, (unsigned)total,
         total > 0 ? 100.0f * used / total : 0.0f);
}

size_t blacklist_hex_to_bytes(const char* hex, uint8_t* out, size_t out_max) {
    if (!hex || !out) return 0;
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0) return 0;
    size_t byte_len = hex_len / 2;
    if (byte_len > out_max) return 0;

    for (size_t i = 0; i < byte_len; ++i) {
        uint8_t hi, lo;
        char ch_hi = hex[i * 2];
        char ch_lo = hex[i * 2 + 1];
        if (ch_hi >= '0' && ch_hi <= '9') hi = ch_hi - '0';
        else if (ch_hi >= 'a' && ch_hi <= 'f') hi = ch_hi - 'a' + 10;
        else if (ch_hi >= 'A' && ch_hi <= 'F') hi = ch_hi - 'A' + 10;
        else return 0;
        if (ch_lo >= '0' && ch_lo <= '9') lo = ch_lo - '0';
        else if (ch_lo >= 'a' && ch_lo <= 'f') lo = ch_lo - 'a' + 10;
        else if (ch_lo >= 'A' && ch_lo <= 'F') lo = ch_lo - 'A' + 10;
        else return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return byte_len;
}
