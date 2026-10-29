/*
 * peer_keys.cpp - senderID → Ed25519 pk 매핑 구현 (SPIFFS 백엔드)
 *
 * 한국어:
 *   매핑 엔트리: 1B active + 8B senderID + 64B pk[2] + 1B key_mask = 74B
 *   32개 매핑 × 74B = 2,368B → /peer_keys.bin
 *
 *   동작:
 *     - boot 시 SPIFFS에서 /peer_keys.bin 로드 → RAM 캐시 g_entries[]
 *     - 매핑 add 시 RAM 갱신 후 SPIFFS 파일에 통째로 write (덮어쓰기)
 *     - 조회는 RAM에서 (빠름, SPIFFS I/O 없음)
 *     - peer_keys_clear()는 SPIFFS 파일 삭제 + RAM 0으로 초기화
 *
 *   NVS → SPIFFS 자동 마이그레이션 (1회):
 *     - 부팅 시 /peer_keys.bin이 없고 NVS namespace "bpkey"의 blob이 있으면
 *       → NVS에서 로드 → SPIFFS로 저장 → NVS 키 삭제
 *     - 이후 모든 동작은 SPIFFS-only
 */

#include "peer_keys.h"
#include "log.h"
#include "bitchat_protocol.h"   // bitchat_id_from_pk

#include <Arduino.h>
#include <SPIFFS.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

// 구버전 호환 - NVS에서 마이그레이션 시 사용
#define BC_PK_NVS_NS  "bpkey"
#define BC_PK_NVS_KEY "keys"

// 매핑 엔트리: 8B senderID + 32B pk × 2 = 74B
typedef struct {
    uint8_t  active;
    uint8_t  sender_id[BC_PEER_KEY_ID_SIZE];
    uint8_t  pk[2][BC_PEER_KEY_PK_SIZE];  // 2 keys: [0]=primary, [1]=secondary
    uint8_t  key_mask;                     // bit 0: pk[0] valid, bit 1: pk[1] valid
} peer_key_entry_t;

// 128개 × 74B = 9,472B. 정적 .bss에 넣으면 dram0_0_seg overflow 발생.
// 따라서 heap (calloc)으로 동적 할당 → 정적 세그먼트 부담 0, heap에서 9.5KB 사용.
// ESP32 heap은 보통 200+KB이므로 충분한 여유.
static peer_key_entry_t* g_entries = nullptr;  // peer_keys_init()에서 calloc
static bool g_init = false;

// SPIFFS에 전체 매핑 저장 (덮어쓰기)
static void persist_to_spiffs(void) {
    File f = SPIFFS.open(BC_PEER_KEYS_FILE, "w");
    if (!f) {
        LOGE("peer_keys: open(%s, w) failed", BC_PEER_KEYS_FILE);
        return;
    }
    size_t written = f.write((const uint8_t*)g_entries, sizeof(g_entries));
    f.close();
    if (written != sizeof(g_entries)) {
        LOGE("peer_keys: write short %u/%u",
             (unsigned)written, (unsigned)sizeof(g_entries));
    }
}

// SPIFFS에서 로드
// 하위 호환: 32-entry 파일(2,368B) → 128-entry 버퍼(9,472B)에 로드 시
//   got=2,368 < sizeof(g_entries)=9,472 → warning + 나머지 zero padding
//   기존 32개 매핑은 정상 로드, 나머지 96개는 inactive(0)로 시작
static bool load_from_spiffs(void) {
    if (!SPIFFS.exists(BC_PEER_KEYS_FILE)) {
        return false;
    }
    File f = SPIFFS.open(BC_PEER_KEYS_FILE, "r");
    if (!f) {
        LOGE("peer_keys: open(%s, r) failed", BC_PEER_KEYS_FILE);
        return false;
    }
    size_t got = f.readBytes((char*)g_entries, sizeof(g_entries));
    f.close();
    if (got != sizeof(g_entries)) {
        // 32-entry → 128-entry 업그레이드 시 발생 (정상 동작)
        LOGW("peer_keys: file size %uB < expected %uB (likely 32-entry upgrade), padding zeros",
             (unsigned)got, (unsigned)sizeof(g_entries));
        if (got < sizeof(g_entries)) {
            memset((uint8_t*)g_entries + got, 0, sizeof(g_entries) - got);
        }
    }
    return true;
}

// NVS에서 로드 (구버전 호환 - 마이그레이션 시 사용)
// 하위 호환: 32-entry blob(2,368B) → 128-entry 버퍼(9,472B)에 로드 시
//   peer_keys_init에서 memset으로 zero 초기화 선행 → NVS blob이 처음 2,368B만 채우고
//   나머지 7,104B는 그대로 0 (inactive) → 정상 동작
static bool load_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(BC_PK_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sz = sizeof(g_entries);
    bool ok = (nvs_get_blob(h, BC_PK_NVS_KEY, g_entries, &sz) == ESP_OK);
    nvs_close(h);
    return ok;
}

// NVS에서 키 삭제 (마이그레이션 완료 후 호출)
static void erase_nvs_keys(void) {
    nvs_handle_t h;
    if (nvs_open(BC_PK_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, BC_PK_NVS_KEY);
    nvs_commit(h);
    nvs_close(h);
    LOGI("peer_keys: erased old NVS blob (migration complete)");
}

bool peer_keys_init(void) {
    if (g_init) return true;

    // Heap에서 g_entries 동적 할당 (정적 .bss 부담 피하기)
    // - 128 entries × 74B = 9,472B
    // - calloc이 0으로 초기화하므로 모든 entry가 inactive 상태로 시작
    if (!g_entries) {
        g_entries = (peer_key_entry_t*)calloc(BC_PEER_KEY_MAX_ENTRIES,
                                              sizeof(peer_key_entry_t));
        if (!g_entries) {
            LOGE("peer_keys: failed to allocate %uB heap for %u entries",
                 (unsigned)(BC_PEER_KEY_MAX_ENTRIES * sizeof(peer_key_entry_t)),
                 BC_PEER_KEY_MAX_ENTRIES);
            return false;
        }
        LOGI("peer_keys: allocated %uB heap for %u entries",
             (unsigned)(BC_PEER_KEY_MAX_ENTRIES * sizeof(peer_key_entry_t)),
             BC_PEER_KEY_MAX_ENTRIES);
    }

    // SPIFFS 마운트는 main.cpp setup()에서 선행됨
    if (!SPIFFS.begin(true)) {
        LOGE("peer_keys: SPIFFS mount failed");
        return false;
    }

    bool loaded = false;
    // 1차: SPIFFS /peer_keys.bin에서 로드
    if (load_from_spiffs()) {
        loaded = true;
        LOGI("peer_keys: loaded from SPIFFS %s", BC_PEER_KEYS_FILE);
    } else {
        // 2차: NVS 마이그레이션 (이전 버전에서 업그레이드한 경우)
        if (load_from_nvs()) {
            loaded = true;
            LOGI("peer_keys: migrated %u mappings from NVS → SPIFFS",
                 BC_PEER_KEY_MAX_ENTRIES);
            // SPIFFS에 저장하고 NVS에서 삭제
            persist_to_spiffs();
            erase_nvs_keys();
        } else {
            LOGI("peer_keys: no existing data, starting empty");
        }
    }

    uint32_t count = 0;
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (g_entries[i].active) count++;
    }
    g_init = true;
    LOGI("peer_keys: initialized (file=%s, %u active/%u max, %uB file)",
         BC_PEER_KEYS_FILE, (unsigned)count, BC_PEER_KEY_MAX_ENTRIES,
         (unsigned)sizeof(g_entries));
    return true;
}

bool peer_keys_add_indexed(const uint8_t* sender_id, const uint8_t* pk, int key_index) {
    if (!g_init) peer_keys_init();
    if (!sender_id || !pk || key_index < 0 || key_index > 1) return false;

    // 기존 매핑 찾기
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (g_entries[i].active &&
            memcmp(g_entries[i].sender_id, sender_id, BC_PEER_KEY_ID_SIZE) == 0) {
            // 이미 같은 키가 있으면 skip
            if (memcmp(g_entries[i].pk[key_index], pk, BC_PEER_KEY_PK_SIZE) == 0 &&
                (g_entries[i].key_mask & (1 << key_index))) {
                return true;  // 중복 add - 변경 없음, SPIFFS write 안 함 (wear 절약)
            }
            memcpy(g_entries[i].pk[key_index], pk, BC_PEER_KEY_PK_SIZE);
            g_entries[i].key_mask |= (1 << key_index);
            persist_to_spiffs();
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
            persist_to_spiffs();
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
    SPIFFS.remove(BC_PEER_KEYS_FILE);
    LOGI("peer_keys: cleared all (SPIFFS file removed)");
}

void peer_keys_debug_dump(void) {
    if (!g_init) return;
    size_t count = peer_keys_count();
    LOGI("=== peer_keys: %u mappings (SPIFFS /peer_keys.bin, %uB) ===",
         (unsigned)count, (unsigned)sizeof(g_entries));
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (!g_entries[i].active) continue;
        char id[17];
        bitchat_id_from_pk(g_entries[i].sender_id, id);
        LOGI("  [%02d] senderID=%s pk=%02x%02x%02x%02x... key_mask=0x%02x",
             i, id,
             g_entries[i].pk[0][0], g_entries[i].pk[0][1],
             g_entries[i].pk[0][2], g_entries[i].pk[0][3],
             g_entries[i].key_mask);
    }
}
