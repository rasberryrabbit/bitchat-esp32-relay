/*
 * peer_keys.cpp - senderID → Ed25519 pk 매핑 구현
 */

#include "peer_keys.h"
#include "log.h"
#include "bitchat_protocol.h"   // bitchat_id_from_pk

#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#define BC_PK_NVS_NS  "bpkey"
#define BC_PK_NVS_KEY "keys"

// 매핑 엔트리: 8B senderID + 32B pk = 40B
typedef struct {
    uint8_t  active;
    uint8_t  sender_id[BC_PEER_KEY_ID_SIZE];
    uint8_t  pk[2][BC_PEER_KEY_PK_SIZE];  // 2 keys: [0]=primary, [1]=secondary
    uint8_t  key_mask;                     // bit 0: pk[0] valid, bit 1: pk[1] valid
} peer_key_entry_t;

static peer_key_entry_t g_entries[BC_PEER_KEY_MAX_ENTRIES];
static bool g_init = false;

static void persist(void) {
    nvs_handle_t h;
    if (nvs_open(BC_PK_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, BC_PK_NVS_KEY, g_entries, sizeof(g_entries));
    nvs_commit(h);
    nvs_close(h);
}

static void load(void) {
    nvs_handle_t h;
    if (nvs_open(BC_PK_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(g_entries);
    if (nvs_get_blob(h, BC_PK_NVS_KEY, g_entries, &sz) != ESP_OK) {
        memset(g_entries, 0, sizeof(g_entries));
    }
    nvs_close(h);
}

bool peer_keys_init(void) {
    if (g_init) return true;
    memset(g_entries, 0, sizeof(g_entries));
    load();
    uint32_t count = 0;
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (g_entries[i].active) count++;
    }
    g_init = true;
    LOGI("peer_keys: loaded %u mappings from NVS", count);
    return true;
}

bool peer_keys_add_indexed(const uint8_t* sender_id, const uint8_t* pk, int key_index) {
    if (!g_init) peer_keys_init();
    if (!sender_id || !pk || key_index < 0 || key_index > 1) return false;

    // 기존 매핑 찾기
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (g_entries[i].active &&
            memcmp(g_entries[i].sender_id, sender_id, BC_PEER_KEY_ID_SIZE) == 0) {
            memcpy(g_entries[i].pk[key_index], pk, BC_PEER_KEY_PK_SIZE);
            g_entries[i].key_mask |= (1 << key_index);
            persist();
            return true;
        }
    }
    // 빈 슬롯
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (!g_entries[i].active) {
            g_entries[i].active = 1;
            memcpy(g_entries[i].sender_id, sender_id, BC_PEER_KEY_ID_SIZE);
            memcpy(g_entries[i].pk[key_index], pk, BC_PEER_KEY_PK_SIZE);
            g_entries[i].key_mask = (1 << key_index);
            persist();
            return true;
        }
    }
    return false;
}

bool peer_keys_get_indexed(const uint8_t* sender_id, int key_index, uint8_t* out_pk) {
    if (!g_init || !sender_id || !out_pk || key_index < 0 || key_index > 1) return false;
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (g_entries[i].active &&
            memcmp(g_entries[i].sender_id, sender_id, BC_PEER_KEY_ID_SIZE) == 0) {
            if (g_entries[i].key_mask & (1 << key_index)) {
                memcpy(out_pk, g_entries[i].pk[key_index], BC_PEER_KEY_PK_SIZE);
                return true;
            }
        }
    }
    return false;
}

bool peer_keys_add(const uint8_t* sender_id, const uint8_t* pk) {
    return peer_keys_add_indexed(sender_id, pk, 0);
}

bool peer_keys_get(const uint8_t* sender_id, uint8_t* out_pk) {
    // index 0 먼저 시도, 없으면 index 1
    if (peer_keys_get_indexed(sender_id, 0, out_pk)) return true;
    return peer_keys_get_indexed(sender_id, 1, out_pk);
}

size_t peer_keys_count(void) {
    if (!g_init) return 0;
    size_t c = 0;
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (g_entries[i].active) c++;
    }
    return c;
}

void peer_keys_clear(void) {
    memset(g_entries, 0, sizeof(g_entries));
    persist();
    LOGI("peer_keys: cleared all");
}

void peer_keys_debug_dump(void) {
    if (!g_init) return;
    size_t count = peer_keys_count();
    LOGI("=== peer_keys: %u mappings ===", count);
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (!g_entries[i].active) continue;
        char id[17];
        bitchat_id_from_pk(g_entries[i].sender_id, id);
        LOGI("  [%02d] senderID=%s pk=%02x%02x%02x%02x...",
             i, id,
             g_entries[i].pk[0], g_entries[i].pk[1],
             g_entries[i].pk[2], g_entries[i].pk[3]);
    }
}
