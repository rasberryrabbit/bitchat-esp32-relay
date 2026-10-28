/*
 * blacklist.cpp - 블랙리스트 필터링 구현
 *
 * 한국어:
 *   pk(32B) 32개 + BLE(6B) 16개를 NVS blob으로 영속화.
 *   - PK: active flag + 32B = 33B × 32 = 1,056B blob
 *   - BLE: active flag + 6B = 7B × 16 = 112B blob
 *   add 시 자동 NVS commit, init 시 NVS에서 load.
 */

#include "blacklist.h"
#include "log.h"
#include "bitchat_protocol.h"   // BITCHAT_PUBKEY_LEN

#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>
#include <stdio.h>

#define BC_BL_NVS_NAMESPACE "bchatbl"
#define BC_BL_NVS_KEY_PKS   "pks"
#define BC_BL_NVS_KEY_BLES  "bles"

// 직렬화 포맷: [active:1B][pk:32B] × 32 = 1,056B
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

// NVS에 저장
static void persist_to_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(BC_BL_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, BC_BL_NVS_KEY_PKS,  g_pk_entries,  sizeof(g_pk_entries));
    nvs_set_blob(h, BC_BL_NVS_KEY_BLES, g_ble_entries, sizeof(g_ble_entries));
    nvs_commit(h);
    nvs_close(h);
}

// NVS에서 로드
static void load_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(BC_BL_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        LOGI("blacklist: no NVS data, starting empty");
        return;
    }
    size_t pk_sz = sizeof(g_pk_entries);
    size_t ble_sz = sizeof(g_ble_entries);
    esp_err_t e1 = nvs_get_blob(h, BC_BL_NVS_KEY_PKS,  g_pk_entries,  &pk_sz);
    esp_err_t e2 = nvs_get_blob(h, BC_BL_NVS_KEY_BLES, g_ble_entries, &ble_sz);
    nvs_close(h);

    // 카운트 계산
    g_pk_count = 0;
    if (e1 == ESP_OK) {
        for (int i = 0; i < BC_BL_MAX_PK_ENTRIES; ++i) {
            if (g_pk_entries[i].active) g_pk_count++;
        }
    } else {
        memset(g_pk_entries, 0, sizeof(g_pk_entries));
    }
    g_ble_count = 0;
    if (e2 == ESP_OK) {
        for (int i = 0; i < BC_BL_MAX_BLE_ENTRIES; ++i) {
            if (g_ble_entries[i].active) g_ble_count++;
        }
    } else {
        memset(g_ble_entries, 0, sizeof(g_ble_entries));
    }
    LOGI("blacklist: loaded %u pk + %u ble from NVS",
         (unsigned)g_pk_count, (unsigned)g_ble_count);
}

bool blacklist_init(void) {
    if (g_init_done) return true;
    memset(g_pk_entries, 0, sizeof(g_pk_entries));
    memset(g_ble_entries, 0, sizeof(g_ble_entries));
    load_from_nvs();
    g_init_done = true;
    g_enabled = true;
    LOGI("blacklist: initialized (enabled=%d, pk=%u, ble=%u)",
         g_enabled, (unsigned)g_pk_count, (unsigned)g_ble_count);
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
            persist_to_nvs();
            char id[BITCHAT_ID_STR_LEN];
            bitchat_id_from_pk(pk, id);
            LOGI("blacklist: added pk id=%s (slot %d, total=%u)",
                 id, i, (unsigned)g_pk_count);
            return true;
        }
    }
    LOGW("blacklist: pk full (%u), cannot add", (unsigned)g_pk_count);
    return false;
}

bool blacklist_remove_pk(const uint8_t* pk) {
    if (!g_init_done || !pk) return false;
    for (int i = 0; i < BC_BL_MAX_PK_ENTRIES; ++i) {
        if (g_pk_entries[i].active &&
            memcmp(g_pk_entries[i].pk, pk, 32) == 0) {
            g_pk_entries[i].active = 0;
            g_pk_count--;
            persist_to_nvs();
            char id[BITCHAT_ID_STR_LEN];
            bitchat_id_from_pk(pk, id);
            LOGI("blacklist: removed pk id=%s (total=%u)",
                 id, (unsigned)g_pk_count);
            return true;
        }
    }
    return false;
}

bool blacklist_is_pk_blocked(const uint8_t* pk) {
    if (!g_init_done || !g_enabled || !pk) return false;
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
            persist_to_nvs();
            char hex[13];
            static const char* h = "0123456789abcdef";
            for (int j = 0; j < 6; ++j) {
                hex[j * 2]     = h[(addr[j] >> 4) & 0xF];
                hex[j * 2 + 1] = h[addr[j] & 0xF];
            }
            hex[12] = 0;
            LOGI("blacklist: added ble %s (slot %d, total=%u)",
                 hex, i, (unsigned)g_ble_count);
            return true;
        }
    }
    LOGW("blacklist: ble full (%u), cannot add", (unsigned)g_ble_count);
    return false;
}

bool blacklist_remove_ble(const uint8_t* addr) {
    if (!g_init_done || !addr) return false;
    for (int i = 0; i < BC_BL_MAX_BLE_ENTRIES; ++i) {
        if (g_ble_entries[i].active &&
            memcmp(g_ble_entries[i].addr, addr, 6) == 0) {
            g_ble_entries[i].active = 0;
            g_ble_count--;
            persist_to_nvs();
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
    persist_to_nvs();
    LOGI("blacklist: cleared all");
}

size_t blacklist_get_pk_count(void)  { return g_pk_count; }
size_t blacklist_get_ble_count(void) { return g_ble_count; }

void blacklist_debug_dump(void) {
    LOGI("=== blacklist dump (enabled=%d) ===", g_enabled);
    LOGI("pk entries: %u/%u", (unsigned)g_pk_count, BC_BL_MAX_PK_ENTRIES);
    for (int i = 0; i < BC_BL_MAX_PK_ENTRIES; ++i) {
        if (!g_pk_entries[i].active) continue;
        char id[BITCHAT_ID_STR_LEN];
        bitchat_id_from_pk(g_pk_entries[i].pk, id);
        LOGI("  pk[%02d] id=%s", i, id);
    }
    LOGI("ble entries: %u/%u", (unsigned)g_ble_count, BC_BL_MAX_BLE_ENTRIES);
    for (int i = 0; i < BC_BL_MAX_BLE_ENTRIES; ++i) {
        if (!g_ble_entries[i].active) continue;
        char hex[13];
        static const char* h = "0123456789abcdef";
        for (int j = 0; j < 6; ++j) {
            hex[j * 2]     = h[(g_ble_entries[i].addr[j] >> 4) & 0xF];
            hex[j * 2 + 1] = h[g_ble_entries[i].addr[j] & 0xF];
        }
        hex[12] = 0;
        LOGI("  ble[%02d] %s", i, hex);
    }
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
