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
    uint8_t  pk[BC_PEER_KEY_PK_SIZE];
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

bool peer_keys_add(const uint8_t* sender_id, const uint8_t* pk) {
    if (!g_init) peer_keys_init();
    if (!sender_id || !pk) return false;

    // 기존 매핑 찾기 (갱신)
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (g_entries[i].active &&
            memcmp(g_entries[i].sender_id, sender_id, BC_PEER_KEY_ID_SIZE) == 0) {
            memcpy(g_entries[i].pk, pk, BC_PEER_KEY_PK_SIZE);
            persist();
            char id[17];
            bitchat_id_from_pk(sender_id, id);
            LOGD("peer_keys: updated pk for senderID=%s", id);
            return true;
        }
    }

    // 빈 슬롯 찾기
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (!g_entries[i].active) {
            g_entries[i].active = 1;
            memcpy(g_entries[i].sender_id, sender_id, BC_PEER_KEY_ID_SIZE);
            memcpy(g_entries[i].pk, pk, BC_PEER_KEY_PK_SIZE);
            persist();
            char id[17];
            bitchat_id_from_pk(sender_id, id);
            LOGI("peer_keys: added pk mapping for senderID=%s (slot %d)", id, i);
            return true;
        }
    }
    LOGW("peer_keys: full, cannot add");
    return false;
}

bool peer_keys_get(const uint8_t* sender_id, uint8_t* out_pk) {
    if (!g_init || !sender_id || !out_pk) return false;
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (g_entries[i].active &&
            memcmp(g_entries[i].sender_id, sender_id, BC_PEER_KEY_ID_SIZE) == 0) {
            memcpy(out_pk, g_entries[i].pk, BC_PEER_KEY_PK_SIZE);
            return true;
        }
    }
    return false;
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
