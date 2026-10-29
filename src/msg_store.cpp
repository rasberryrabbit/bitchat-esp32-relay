/*
 * msg_store.cpp - SPIFFS overflow cache 구현
 *
 * 한국어:
 *   단일 파일 /msg_store.bin에 고정 크기 레코드 64개를 저장.
 *   msg_cache(RAM)이 가득 찼을 때 가장 오래된 entry를 이쪽으로 밀어냄.
 *
 *   레코드 구조 (264 bytes):
 *     [0]    active (0/1)
 *     [1-4]  timestamp_ms (little-endian uint32, millis())
 *     [5-6]  pkt_len (little-endian uint16, ≤ 256)
 *     [7-262] pkt_data (256 bytes)
 *     [263]  reserved (padding)
 *
 *   동작:
 *     add: 빈 슬롯 찾기 → 없으면 가장 오래된 ts 슬롯에 in-place write
 *     load_to_ram: 파일 전체 읽기 → 모든 active 레코드 emit 후 파일 truncate
 *     flush_to_callback: load_to_ram과 동일하나 emit_fn이 NULL이어도 truncate
 *     clear: 파일 삭제
 *
 *   플래시 마모 최소화:
 *     - 64개 슬롯 순환 갱신 (FIFO) → 한 슬롯에만 반복 write 안 함
 *     - 파일 truncate는 레코드 active=0로 마킹하는 것보다
 *       빠르지만 한 번의 erase 발생 → load_to_ram/flush 시에만 수행
 */

#include "msg_store.h"
#include "log.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <string.h>

// 레코드 오프셋 계산 (i번째 레코드의 시작 바이트)
#define RECORD_OFFSET(i)  ((i) * MSG_STORE_RECORD_SIZE)

#pragma pack(push, 1)
typedef struct {
    uint8_t  active;          // [0]
    uint32_t timestamp_ms;    // [1-4]
    uint16_t pkt_len;         // [5-6]
    uint8_t  pkt_data[MSG_STORE_MAX_PKT_LEN];  // [7-262]
    uint8_t  reserved;        // [263] padding
} msg_store_record_t;
#pragma pack(pop)

static_assert(sizeof(msg_store_record_t) == MSG_STORE_RECORD_SIZE,
              "msg_store_record_t size mismatch");

static bool g_init = false;
static uint32_t g_total_added = 0;
static uint32_t g_total_loaded = 0;
static uint32_t g_total_flushed = 0;
static uint32_t g_total_dropped_oversize = 0;

// Circular buffer write cursor (RAM cache, 부팅 시 1회 scan에서 설정)
// add() 시 이 슬롯에 write하고 +1 mod MAX. 빈 슬롯 우선, 모두 찼으면 가장 오래된 ts.
static size_t g_next_write_slot = 0;
static size_t g_active_count_cache = 0;  // 부팅 시 scan으로 설정, add 시 갱신

// 부팅 시 1회 파일 전체 scan → 다음 write 슬롯 + 활성 레코드 수 캐싱
// 3072 records × 264B = 792KB → 한 번 scan하는 데 약 200-300ms
static void scan_and_cache_write_slot(void) {
    g_next_write_slot = 0;
    g_active_count_cache = 0;

    if (!SPIFFS.exists(MSG_STORE_FILE)) return;

    File f = SPIFFS.open(MSG_STORE_FILE, "r");
    if (!f) return;

    size_t file_size = f.size();
    size_t record_count = file_size / MSG_STORE_RECORD_SIZE;
    if (record_count == 0) {
        f.close();
        return;
    }

    int first_empty = -1;
    uint32_t oldest_ts = 0xFFFFFFFF;
    size_t oldest_idx = 0;
    msg_store_record_t rec;

    for (size_t i = 0; i < record_count && i < MSG_STORE_MAX_ENTRIES; ++i) {
        f.seek(RECORD_OFFSET(i), SeekSet);
        if (f.readBytes((char*)&rec, sizeof(rec)) != sizeof(rec)) break;
        if (!rec.active) {
            if (first_empty < 0) first_empty = (int)i;
        } else {
            g_active_count_cache++;
            if (rec.timestamp_ms < oldest_ts) {
                oldest_ts = rec.timestamp_ms;
                oldest_idx = i;
            }
        }
    }
    f.close();

    if (first_empty >= 0) {
        g_next_write_slot = (size_t)first_empty;
    } else if (g_active_count_cache > 0) {
        // 모든 슬롯이 active → 가장 오래된 ts 슬롯부터 덮어쓰기
        g_next_write_slot = oldest_idx;
    } else {
        g_next_write_slot = 0;
    }

    LOGI("msg_store: scan done - active=%u/%u, next_write_slot=%u",
         (unsigned)g_active_count_cache, MSG_STORE_MAX_ENTRIES,
         (unsigned)g_next_write_slot);
}

bool msg_store_init(void) {
    if (g_init) return true;
    // SPIFFS 마운트는 main.cpp setup()에서 선행됨
    if (!SPIFFS.begin(true)) {
        LOGE("msg_store: SPIFFS mount failed");
        return false;
    }
    g_init = true;

    // 부팅 시 1회 scan → 다음 write slot + active count 캐싱
    scan_and_cache_write_slot();

    LOGI("msg_store: initialized (file=%s, max=%u records, %uB/record, total=%uB)",
         MSG_STORE_FILE, MSG_STORE_MAX_ENTRIES,
         MSG_STORE_RECORD_SIZE,
         MSG_STORE_MAX_ENTRIES * MSG_STORE_RECORD_SIZE);
    return true;
}

size_t msg_store_count(void) {
    if (!g_init) return 0;
    return g_active_count_cache;
}

bool msg_store_add(const uint8_t* data, size_t len) {
    if (!g_init) msg_store_init();
    if (!g_init) return false;
    if (!data || len == 0) return false;
    if (len > MSG_STORE_MAX_PKT_LEN) {
        g_total_dropped_oversize++;
        LOGD("msg_store: drop oversize pkt len=%u (max=%u)",
             (unsigned)len, MSG_STORE_MAX_PKT_LEN);
        return false;
    }

    // 파일이 없으면 빈 파일 생성
    bool existed = SPIFFS.exists(MSG_STORE_FILE);
    File f = SPIFFS.open(MSG_STORE_FILE, existed ? "r+" : "w+");
    if (!f) {
        LOGE("msg_store: open(%s) failed", MSG_STORE_FILE);
        return false;
    }

    // Circular buffer: g_next_write_slot에 직접 write (부팅 시 scan에서 설정)
    // full scan 안 함 → 3072 records에서도 O(1)
    size_t target_slot = g_next_write_slot;

    // 기존 슬롯이 active였으면 카운트 조정 (overwrite)
    msg_store_record_t old_rec;
    f.seek(RECORD_OFFSET(target_slot), SeekSet);
    bool was_active = false;
    if (f.readBytes((char*)&old_rec, sizeof(old_rec)) == sizeof(old_rec)) {
        if (old_rec.active) was_active = true;
    }

    // 새 레코드 작성
    msg_store_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.active = 1;
    rec.timestamp_ms = millis();
    rec.pkt_len = (uint16_t)len;
    memcpy(rec.pkt_data, data, len);

    f.seek(RECORD_OFFSET(target_slot), SeekSet);
    size_t written = f.write((const uint8_t*)&rec, sizeof(rec));
    f.close();

    if (written != sizeof(rec)) {
        LOGE("msg_store: write short %u/%u at slot %u",
             (unsigned)written, (unsigned)sizeof(rec), (unsigned)target_slot);
        return false;
    }

    // 캐시 갱신
    if (!was_active) g_active_count_cache++;
    // 다음 write slot (circular)
    g_next_write_slot = (target_slot + 1) % MSG_STORE_MAX_ENTRIES;

    g_total_added++;
    LOGD("msg_store: added (slot=%u, len=%u, total_added=%u, active=%u/%u)",
         (unsigned)target_slot, (unsigned)len, g_total_added,
         (unsigned)g_active_count_cache, MSG_STORE_MAX_ENTRIES);
    return true;
}

size_t msg_store_load_to_ram(msg_store_emit_fn emit_fn) {
    if (!g_init) msg_store_init();
    if (!g_init) return 0;

    if (!SPIFFS.exists(MSG_STORE_FILE)) {
        return 0;  // 파일 없음 - 정상
    }

    File f = SPIFFS.open(MSG_STORE_FILE, "r+");
    if (!f) {
        LOGE("msg_store: load open failed");
        return 0;
    }

    size_t loaded = 0;
    size_t kept = 0;  // RAM 가득 차서 SPIFFS에 남긴 레코드 수
    msg_store_record_t rec;
    size_t record_count = f.size() / MSG_STORE_RECORD_SIZE;
    for (size_t i = 0; i < record_count && i < MSG_STORE_MAX_ENTRIES; ++i) {
        f.seek(RECORD_OFFSET(i), SeekSet);
        if (f.readBytes((char*)&rec, sizeof(rec)) != sizeof(rec)) break;
        if (!rec.active) continue;
        if (rec.pkt_len == 0 || rec.pkt_len > MSG_STORE_MAX_PKT_LEN) continue;

        bool consumed = false;
        if (emit_fn) {
            consumed = emit_fn(rec.pkt_data, rec.pkt_len);
        }

        if (consumed) {
            // RAM에 적재 성공 → SPIFFS에서 inactive 마킹
            rec.active = 0;
            f.seek(RECORD_OFFSET(i), SeekSet);
            f.write((const uint8_t*)&rec, sizeof(rec));
            loaded++;
            g_active_count_cache--;  // 캐시 갱신
        } else {
            kept++;
        }
    }
    f.close();

    if (loaded > 0 || kept > 0) {
        g_total_loaded += loaded;
        LOGI("msg_store: loaded %u records to RAM, %u kept in SPIFFS (RAM full)",
             (unsigned)loaded, (unsigned)kept);
    }

    return loaded;
}

size_t msg_store_flush_to_callback(void (*emit_fn)(const uint8_t* data, size_t len)) {
    if (!g_init) msg_store_init();
    if (!g_init) return 0;

    if (!SPIFFS.exists(MSG_STORE_FILE)) {
        return 0;  // 파일 없음 - 정상
    }

    File f = SPIFFS.open(MSG_STORE_FILE, "r");
    if (!f) {
        LOGE("msg_store: flush open failed");
        return 0;
    }

    size_t flushed = 0;
    msg_store_record_t rec;
    size_t record_count = f.size() / MSG_STORE_RECORD_SIZE;
    for (size_t i = 0; i < record_count && i < MSG_STORE_MAX_ENTRIES; ++i) {
        f.seek(RECORD_OFFSET(i), SeekSet);
        if (f.readBytes((char*)&rec, sizeof(rec)) != sizeof(rec)) break;
        if (!rec.active) continue;
        if (rec.pkt_len == 0 || rec.pkt_len > MSG_STORE_MAX_PKT_LEN) continue;

        if (emit_fn) {
            emit_fn(rec.pkt_data, rec.pkt_len);
        }
        flushed++;
    }
    f.close();

    // flush 완료 후 파일 truncate (모든 레코드 제거)
    if (flushed > 0) {
        SPIFFS.remove(MSG_STORE_FILE);
        g_total_flushed += flushed;
        g_active_count_cache = 0;  // 캐시 초기화
        g_next_write_slot = 0;     // 다음 write는 slot 0부터
        LOGI("msg_store: flushed %u records via callback (total_flushed=%u)",
             (unsigned)flushed, g_total_flushed);
    }
    return flushed;
}

bool msg_store_clear(void) {
    if (!g_init) msg_store_init();
    if (!g_init) return false;
    bool ok = SPIFFS.remove(MSG_STORE_FILE);
    // 캐시 초기화
    g_active_count_cache = 0;
    g_next_write_slot = 0;
    LOGI("msg_store: cleared (file %s)",
         SPIFFS.exists(MSG_STORE_FILE) ? "still exists" : "removed");
    return ok;
}

void msg_store_debug_dump(void) {
    if (!g_init) {
        LOGI("msg_store: not initialized");
        return;
    }
    size_t count = msg_store_count();
    size_t spiffs_total = SPIFFS.totalBytes();
    size_t spiffs_used = SPIFFS.usedBytes();
    LOGI("=== msg_store dump ===");
    LOGI("file: %s", MSG_STORE_FILE);
    LOGI("active records: %u/%u (%.1f%%)",
         (unsigned)count, MSG_STORE_MAX_ENTRIES,
         100.0f * count / MSG_STORE_MAX_ENTRIES);
    LOGI("spiffs usage: %uB / %uB (%.1f%%)",
         (unsigned)spiffs_used, (unsigned)spiffs_total,
         spiffs_total > 0 ? 100.0f * spiffs_used / spiffs_total : 0.0f);
    LOGI("stats: added=%u loaded=%u flushed=%u dropped_oversize=%u",
         (unsigned)g_total_added, (unsigned)g_total_loaded,
         (unsigned)g_total_flushed, (unsigned)g_total_dropped_oversize);
    // 각 레코드 요약
    if (count > 0 && SPIFFS.exists(MSG_STORE_FILE)) {
        File f = SPIFFS.open(MSG_STORE_FILE, "r");
        if (f) {
            msg_store_record_t rec;
            size_t record_count = f.size() / MSG_STORE_RECORD_SIZE;
            for (size_t i = 0; i < record_count && i < MSG_STORE_MAX_ENTRIES; ++i) {
                f.seek(RECORD_OFFSET(i), SeekSet);
                if (f.readBytes((char*)&rec, sizeof(rec)) != sizeof(rec)) break;
                if (!rec.active) continue;
                uint32_t age_s = (millis() - rec.timestamp_ms) / 1000;
                LOGI("  slot[%u] len=%u age=%lus",
                     (unsigned)i, (unsigned)rec.pkt_len, age_s);
            }
            f.close();
        }
    }
}
