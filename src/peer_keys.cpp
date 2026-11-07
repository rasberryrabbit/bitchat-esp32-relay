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
#include <esp_heap_caps.h>
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

// v43: 256개 × 74B = 18,944B. calloc으로 동적 할당.
// v43: LRU 추적용 last_seen 배열 (RAM 전용, 동적 할당 → .bss 오버플로우 방지)
// v44: 뮤텍스 + 지연 쓰기 + 이중 쓰기 (.bak 백업)
static uint32_t* g_last_seen = nullptr;
static peer_key_entry_t* g_entries = nullptr;
static bool g_init = false;
static SemaphoreHandle_t g_mutex = nullptr;      // v44: 동시성 보호
static volatile bool g_dirty = false;             // v44: 지연 쓰기 플래그
#define BC_PEER_KEYS_BAK_FILE "/peer_keys.bak"   // v44: 백업 파일

// g_entries 전체 버퍼 크기
#define G_ENTRIES_BYTES ((size_t)(BC_PEER_KEY_MAX_ENTRIES * sizeof(peer_key_entry_t)))

// v44: 이중 쓰기 — .bak 먼저, .bin 나중 (전원 차단 시 .bak에서 복구)
static void persist_to_spiffs(void) {
    // 1단계: 백업 파일에 쓰기
    File bak = SPIFFS.open(BC_PEER_KEYS_BAK_FILE, "w");
    if (bak) {
        size_t written = bak.write((const uint8_t*)g_entries, G_ENTRIES_BYTES);
        bak.close();
        if (written != G_ENTRIES_BYTES) {
            LOGE("peer_keys: bak write short %u/%u", (unsigned)written, (unsigned)G_ENTRIES_BYTES);
            return;  // 백업 실패 시 원본 쓰지 않음 (안전)
        }
    } else {
        LOGE("peer_keys: open(%s, w) failed", BC_PEER_KEYS_BAK_FILE);
        return;
    }
    // 2단계: 원본 파일에 쓰기
    File f = SPIFFS.open(BC_PEER_KEYS_FILE, "w");
    if (!f) {
        LOGE("peer_keys: open(%s, w) failed", BC_PEER_KEYS_FILE);
        return;
    }
    size_t written = f.write((const uint8_t*)g_entries, G_ENTRIES_BYTES);
    f.close();
    if (written != G_ENTRIES_BYTES) {
        LOGE("peer_keys: write short %u/%u", (unsigned)written, (unsigned)G_ENTRIES_BYTES);
    }
}

// SPIFFS에서 로드
// 하위 호환: 32-entry 파일(2,368B) → 128-entry 버퍼(9,472B)에 로드 시
//   got=2,368 < G_ENTRIES_BYTES=9,472 → warning + 나머지 zero padding
//   기존 32개 매핑은 정상 로드, 나머지 96개는 inactive(0)로 시작
//
// 손상 감지: 파일 크기가 1 entry(74B)보다 작으면 corrupt로 간주
//   → sizeof 버그로 4 bytes만 기록된 파일 등
//   → false 반환 → peer_keys_init에서 NVS 마이그레이션 경로로 진입
// v44: 백업에서 로드 (.bin 손상 시 .bak에서 복구)
static bool load_from_file(const char* path) {
    if (!SPIFFS.exists(path)) return false;
    File f = SPIFFS.open(path, "r");
    if (!f) return false;
    size_t file_size = f.size();
    if (file_size < sizeof(peer_key_entry_t)) {
        f.close();
        return false;
    }
    size_t got = f.readBytes((char*)g_entries, G_ENTRIES_BYTES);
    f.close();
    if (got < G_ENTRIES_BYTES) {
        memset((uint8_t*)g_entries + got, 0, G_ENTRIES_BYTES - got);
    }
    return true;
}

static bool load_from_spiffs(void) {
    // v44: 1차 — 원본 파일 로드 시도
    if (load_from_file(BC_PEER_KEYS_FILE)) {
        LOGI("peer_keys: loaded from %s", BC_PEER_KEYS_FILE);
        return true;
    }
    // v44: 2차 — 백업 파일에서 복구
    if (SPIFFS.exists(BC_PEER_KEYS_BAK_FILE)) {
        LOGW("peer_keys: %s corrupt, restoring from %s", BC_PEER_KEYS_FILE, BC_PEER_KEYS_BAK_FILE);
        if (load_from_file(BC_PEER_KEYS_BAK_FILE)) {
            // 백업에서 복구 성공 → 원본 파일 재생성
            persist_to_spiffs();
            LOGI("peer_keys: restored from backup, primary file recreated");
            return true;
        }
    }
    return false;
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
    size_t sz = G_ENTRIES_BYTES;
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
    // - v37: S3에서는 PSRAM에서 할당 (heap_caps_calloc with SPIRAM cap)
    //   C3/ESP32: PSRAM 없음 → 일반 calloc (내부 RAM)
    if (!g_entries) {
        // v39e: peer_keys는 내부 RAM으로 복귀 (v38b 안정 설정)
        //   PSRAM 할당 시 heap 단편화 → cert gen 크래시 유발
        g_entries = (peer_key_entry_t*)calloc(BC_PEER_KEY_MAX_ENTRIES,
                                              sizeof(peer_key_entry_t));
        if (!g_entries) {
            LOGE("peer_keys: failed to allocate %uB heap for %u entries",
                 (unsigned)(BC_PEER_KEY_MAX_ENTRIES * sizeof(peer_key_entry_t)),
                 BC_PEER_KEY_MAX_ENTRIES);
            return false;
        }
        LOGI("peer_keys: allocated %uB heap for %u entries",
             (unsigned)G_ENTRIES_BYTES, BC_PEER_KEY_MAX_ENTRIES);
    }

    // v43: LRU 추적 배열 동적 할당 + 초기화
    if (!g_last_seen) {
        g_last_seen = (uint32_t*)calloc(BC_PEER_KEY_MAX_ENTRIES, sizeof(uint32_t));
    }

    // v44: 뮤텍스 생성
    if (!g_mutex) {
        g_mutex = xSemaphoreCreateMutex();
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
         (unsigned)G_ENTRIES_BYTES);
    return true;
}

bool peer_keys_add_indexed(const uint8_t* sender_id, const uint8_t* pk, int key_index) {
    if (!g_init) peer_keys_init();
    if (!sender_id || !pk || key_index < 0 || key_index > 1) return false;

    // v44: 뮤텍스 보호 (BLE 콜백 동시성 방지)
    if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY);

    bool changed = false;
    // 기존 매핑 찾기
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (g_entries[i].active &&
            memcmp(g_entries[i].sender_id, sender_id, BC_PEER_KEY_ID_SIZE) == 0) {
            if (memcmp(g_entries[i].pk[key_index], pk, BC_PEER_KEY_PK_SIZE) == 0 &&
                (g_entries[i].key_mask & (1 << key_index))) {
                g_last_seen[i] = millis();  // 중복이어도 LRU 갱신
                if (g_mutex) xSemaphoreGive(g_mutex);
                return true;
            }
            memcpy(g_entries[i].pk[key_index], pk, BC_PEER_KEY_PK_SIZE);
            g_entries[i].key_mask |= (1 << key_index);
            g_last_seen[i] = millis();
            changed = true;
            break;
        }
    }

    if (!changed) {
        // 빈 슬롯
        for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
            if (!g_entries[i].active) {
                g_entries[i].active = 1;
                memcpy(g_entries[i].sender_id, sender_id, BC_PEER_KEY_ID_SIZE);
                memcpy(g_entries[i].pk[key_index], pk, BC_PEER_KEY_PK_SIZE);
                g_entries[i].key_mask = (1 << key_index);
                g_last_seen[i] = millis();
                changed = true;
                break;
            }
        }
    }

    if (!changed) {
        // v43: LRU 교체
        int oldest = 0;
        uint32_t oldest_time = g_last_seen[0];
        for (int i = 1; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
            if (g_last_seen[i] < oldest_time) {
                oldest_time = g_last_seen[i];
                oldest = i;
            }
        }
        LOGW("peer_keys: full (%d), evicting oldest slot %d",
             BC_PEER_KEY_MAX_ENTRIES, oldest);
        memset(&g_entries[oldest], 0, sizeof(peer_key_entry_t));
        g_entries[oldest].active = 1;
        memcpy(g_entries[oldest].sender_id, sender_id, BC_PEER_KEY_ID_SIZE);
        memcpy(g_entries[oldest].pk[key_index], pk, BC_PEER_KEY_PK_SIZE);
        g_entries[oldest].key_mask = (1 << key_index);
        g_last_seen[oldest] = millis();
        changed = true;
    }

    // v44: 지연 쓰기 — 즉시 persist하지 않고 dirty 플래그만 설정
    if (changed) {
        g_dirty = true;
    }

    if (g_mutex) xSemaphoreGive(g_mutex);
    return changed;
}

// v44: 지연 쓰기 flush — main loop에서 주기적 호출
void peer_keys_flush(void) {
    if (!g_init || !g_dirty) return;
    if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_dirty) {
        persist_to_spiffs();
        g_dirty = false;
    }
    if (g_mutex) xSemaphoreGive(g_mutex);
}

bool peer_keys_get_indexed(const uint8_t* sender_id, int key_index, uint8_t* out_pk) {
    if (!g_init || !sender_id || !out_pk || key_index < 0 || key_index > 1) return false;
    for (int i = 0; i < BC_PEER_KEY_MAX_ENTRIES; ++i) {
        if (g_entries[i].active &&
            memcmp(g_entries[i].sender_id, sender_id, BC_PEER_KEY_ID_SIZE) == 0) {
            if (g_entries[i].key_mask & (1 << key_index)) {
                memcpy(out_pk, g_entries[i].pk[key_index], BC_PEER_KEY_PK_SIZE);
                g_last_seen[i] = millis();  // v43: LRU 갱신 (조회 시 접근 시간 기록)
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
    memset(g_entries, 0, G_ENTRIES_BYTES);
    SPIFFS.remove(BC_PEER_KEYS_FILE);
    LOGI("peer_keys: cleared all (SPIFFS file removed)");
}

void peer_keys_debug_dump(void) {
    if (!g_init) return;
    size_t count = peer_keys_count();
    LOGI("=== peer_keys: %u mappings (SPIFFS /peer_keys.bin, %uB) ===",
         (unsigned)count, (unsigned)G_ENTRIES_BYTES);
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
