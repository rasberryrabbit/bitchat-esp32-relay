/*
 * storage.cpp - NVS persistence implementation
 *
 * 한국어:
 *   nvs_flash를 초기화하고, 자체 Ed25519 96바이트 키쌍을 영속화.
 *   - priv (64B) + pub (32B) 두 개의 NVS blob key에 분리 저장
 *   - 매 5회 카운터 증가마다 NVS 동기화 (플래시 마모 분산)
 */

#include "storage.h"
#include "log.h"
#include "bitchat_protocol.h"

#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

static const char* TAG = "storage";

bool storage_init(bitchat_storage_t* st) {
    if (!st) return false;
    memset(st, 0, sizeof(*st));

    // 1. NVS 초기화 (이미 되어있으면 무시)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        LOGW("NVS: corrupt/version mismatch, erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        LOGE("NVS init failed: 0x%04x", err);
        return false;
    }

    nvs_handle_t h;
    err = nvs_open(BC_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        LOGE("NVS open failed: 0x%04x", err);
        return false;
    }

    // 2. 자체 키쌍 불러오기 (priv 64B + pub 32B)
    uint8_t priv[64];
    uint8_t pub[32];
    size_t priv_len = sizeof(priv);
    size_t pub_len  = sizeof(pub);
    err = nvs_get_blob(h, BC_NVS_KEY_PRIV, priv, &priv_len);
    esp_err_t err_pub = nvs_get_blob(h, BC_NVS_KEY_PUB,  pub,  &pub_len);
    bool need_gen = false;
    if (err != ESP_OK || err_pub != ESP_OK ||
        priv_len != 64 || pub_len != 32) {
        need_gen = true;
    }

    if (need_gen) {
        LOGI("no self keypair in NVS, generating new...");
        if (!crypto_generate_keypair(&st->self_kp)) {
            nvs_close(h);
            return false;
        }
        // NVS에 저장
        err = nvs_set_blob(h, BC_NVS_KEY_PRIV,
                           st->self_kp.private_key, 64);
        if (err != ESP_OK) {
            LOGE("NVS set priv failed: 0x%04x", err);
        }
        err = nvs_set_blob(h, BC_NVS_KEY_PUB,
                           st->self_kp.public_key, 32);
        if (err != ESP_OK) {
            LOGE("NVS set pub failed: 0x%04x", err);
        }
        // 최초 counter = 0
        st->self_counter = 0;
        nvs_set_u64(h, BC_NVS_KEY_COUNTER, 0);
        nvs_commit(h);
        LOGI("self keypair persisted to NVS");
    } else {
        if (!crypto_keypair_from_storage(&st->self_kp, priv, pub)) {
            LOGE("crypto_keypair_from_storage failed");
            nvs_close(h);
            return false;
        }
        uint64_t ctr = 0;
        nvs_get_u64(h, BC_NVS_KEY_COUNTER, &ctr);
        st->self_counter = ctr;
        LOGI("self keypair restored from NVS, counter=%llu",
             (unsigned long long)ctr);
    }

    // 3. 피어 목록 불러오기 (v2 포맷 체크 - 8B senderID 마이그레이션)
    uint32_t peer_ver = 0;
    nvs_get_u32(h, "pver", &peer_ver);
    if (peer_ver != 2) {
        LOGI("peer cache: version %u != 2, clearing old 32B-format data", peer_ver);
        nvs_erase_key(h, BC_NVS_KEY_PEERS);
        nvs_set_u32(h, "pver", 2);
        nvs_commit(h);
        st->peer_count = 0;
    } else {
        size_t peers_sz = 0;
        err = nvs_get_blob(h, BC_NVS_KEY_PEERS, NULL, &peers_sz);
        if (err == ESP_OK && peers_sz > 0 && peers_sz <= sizeof(st->peers)) {
            nvs_get_blob(h, BC_NVS_KEY_PEERS, st->peers, &peers_sz);
            st->peer_count = (uint8_t)(peers_sz / sizeof(bitchat_peer_t));
            LOGI("loaded %u peers from NVS (v2, 8B senderID)", st->peer_count);
        } else {
            st->peer_count = 0;
        }
    }

    st->ready = true;
    nvs_close(h);
    return true;
}

const bitchat_keypair_t* storage_self_keypair(const bitchat_storage_t* st) {
    if (!st || !st->ready) return nullptr;
    return &st->self_kp;
}

uint64_t storage_next_counter(bitchat_storage_t* st) {
    if (!st || !st->ready) return 0;
    st->self_counter++;
    // 매 5회마다 NVS 동기화 - 플래시 마모 분산
    static uint8_t lazy_counter = 0;
    if (++lazy_counter >= 5) {
        lazy_counter = 0;
        nvs_handle_t h;
        if (nvs_open(BC_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u64(h, BC_NVS_KEY_COUNTER, st->self_counter);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    return st->self_counter;
}

bool storage_add_peer(bitchat_storage_t* st, const uint8_t* pk) {
    if (!st || !pk) return false;
    if (storage_has_peer(st, pk)) return false;
    if (st->peer_count >= BC_MAX_PEERS) {
        // 가장 오래된 피어 제거 (FIFO) - 인덱스 0번 삭제 후 시프트
        memmove(&st->peers[0], &st->peers[1],
                sizeof(bitchat_peer_t) * (BC_MAX_PEERS - 1));
        st->peer_count = BC_MAX_PEERS - 1;
    }
    memcpy(st->peers[st->peer_count].sender_id, pk, 8);
    st->peer_count++;
    // NVS에 동기화 (한 번에 bulk write)
    nvs_handle_t h;
    if (nvs_open(BC_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, BC_NVS_KEY_PEERS, st->peers,
                     sizeof(bitchat_peer_t) * st->peer_count);
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

bool storage_has_peer(const bitchat_storage_t* st, const uint8_t* pk) {
    if (!st || !pk) return false;
    for (uint8_t i = 0; i < st->peer_count; ++i) {
        if (memcmp(st->peers[i].sender_id, pk, 8) == 0) return true;
    }
    return false;
}

void storage_debug_dump(const bitchat_storage_t* st) {
    if (!st) return;
    char id[BITCHAT_ID_STR_LEN];
    bitchat_id_from_pk(st->self_kp.public_key, id);
    LOGI("=== storage dump ===");
    LOGI("self_id=%s ready=%d counter=%llu peers=%u",
         id, st->ready, (unsigned long long)st->self_counter, st->peer_count);
    for (uint8_t i = 0; i < st->peer_count; ++i) {
        bitchat_id_from_pk(st->peers[i].sender_id, id);
        LOGI("peer[%u] id=%s", i, id);
    }
    // 배터리 무시 플래그 상태 표시
    LOGI("battery_ignore=%s", storage_get_battery_ignore() ? "ON" : "OFF");
    LOGI("spiffs_cache_enabled=%s", storage_get_spiffs_cache_enabled() ? "ON" : "OFF");
    LOGI("deep_sleep_enabled=%s", storage_get_deep_sleep_enabled() ? "ON" : "OFF");
}

// ----- 배터리 무시 플래그 (NVS 영속화) -----

bool storage_get_battery_ignore(void) {
    nvs_handle_t h;
    if (nvs_open(BC_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;  // NVS 열기 실패 시 기본값(false)
    }
    uint8_t val = 0;
    esp_err_t err = nvs_get_u8(h, BC_NVS_KEY_BAT_IGN, &val);
    nvs_close(h);
    if (err != ESP_OK) {
        // 키가 없으면 첫 부팅 - 기본값 false
        return false;
    }
    return val != 0;
}

bool storage_set_battery_ignore(bool ignore) {
    nvs_handle_t h;
    if (nvs_open(BC_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        LOGE("storage: NVS open failed for battery_ignore");
        return false;
    }
    uint8_t val = ignore ? 1 : 0;
    esp_err_t err = nvs_set_u8(h, BC_NVS_KEY_BAT_IGN, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        LOGE("storage: NVS set battery_ignore failed: 0x%04x", err);
        return false;
    }
    LOGI("storage: battery_ignore=%s (persisted to NVS)",
         ignore ? "ON" : "OFF");
    return true;
}

// ----- store-and-forward SPIFFS 캐시 enable 플래그 -----

bool storage_get_spiffs_cache_enabled(void) {
    nvs_handle_t h;
    if (nvs_open(BC_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return true;  // NVS 열기 실패 시 기본값 ON
    }
    uint8_t val = 1;  // 기본값
    esp_err_t err = nvs_get_u8(h, BC_NVS_KEY_SFF_EN, &val);
    nvs_close(h);
    if (err != ESP_OK) {
        // 키가 없으면 첫 부팅 → 기본값 ON
        return true;
    }
    return val != 0;
}

bool storage_set_spiffs_cache_enabled(bool enabled) {
    nvs_handle_t h;
    if (nvs_open(BC_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        LOGE("storage: NVS open failed for spiffs_cache_enabled");
        return false;
    }
    uint8_t val = enabled ? 1 : 0;
    esp_err_t err = nvs_set_u8(h, BC_NVS_KEY_SFF_EN, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        LOGE("storage: NVS set spiffs_cache_enabled failed: 0x%04x", err);
        return false;
    }
    LOGI("storage: spiffs_cache_enabled=%s (persisted to NVS)",
         enabled ? "ON" : "OFF");
    return true;
}

// ----- 딥슬립 허용 플래그 -----

bool storage_get_deep_sleep_enabled(void) {
    nvs_handle_t h;
    if (nvs_open(BC_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;  // NVS 열기 실패 시 기본값 OFF
    }
    uint8_t val = 0;  // 기본값 OFF
    esp_err_t err = nvs_get_u8(h, BC_NVS_KEY_DS_EN, &val);
    nvs_close(h);
    if (err != ESP_OK) {
        // 키가 없으면 첫 부팅 → 기본값 OFF
        return false;
    }
    return val != 0;
}

bool storage_set_deep_sleep_enabled(bool enabled) {
    nvs_handle_t h;
    if (nvs_open(BC_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        LOGE("storage: NVS open failed for deep_sleep_enabled");
        return false;
    }
    uint8_t val = enabled ? 1 : 0;
    esp_err_t err = nvs_set_u8(h, BC_NVS_KEY_DS_EN, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        LOGE("storage: NVS set deep_sleep_enabled failed: 0x%04x", err);
        return false;
    }
    LOGI("storage: deep_sleep_enabled=%s (persisted to NVS)",
         enabled ? "ON" : "OFF");
    return true;
}
