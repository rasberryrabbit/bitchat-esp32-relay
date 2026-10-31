/*
 * lora_relay.cpp - LoRa 자동 감지 + BLE↔LoRa 브릿지 구현
 *
 * 한국어:
 *   다양한 ESP32+LoRa 통합 보드의 핀아웃을 시도하여 SX127x를 자동 인식.
 *   - Heltec WiFi LoRa 32 (V2): SCK=5, MISO=19, MOSI=27, CS=18, RST=14, IRQ=26
 *   - TTGO LoRa32 V1: SCK=5, MISO=19, MOSI=27, CS=18, RST=14, IRQ=26
 *   - TTGO T-Beam: SCK=5, MISO=19, MOSI=27, CS=18, RST=23, IRQ=26
 *   - Heltec Wireless Stick: SCK=5, MISO=19, MOSI=27, CS=18, RST=14, IRQ=26
 *   - 일반 SPI pins (ESP32 default SPI 18/19/23 + 임의 CS/RST/IRQ)
 *
 *   인식 알고리즘:
 *     1) SPI init (sck, miso, mosi)
 *     2) LoRa.setPins(cs, rst, irq)
 *     3) LoRa.begin(freq) -> 1이면 OK
 *     4) RegVersion (0x42) 읽기 -> 0x12면 SX1276 정상
 */

#include "lora_relay.h"
#include "log.h"
#include "ble_relay.h"   // g_relay.enqueue_packet 사용
#include "packet_splitter.h"   // 큰 패킷 분할/재조립

#include <string.h>

// SX127x RegVersion register
#define SX127X_REG_VERSION 0x42
#define SX1276_VERSION_ID   0x12
#define SX1272_VERSION_ID   0x22

// 자동 감지할 핀아웃 후보들
static const lora_pinout_t kPinouts[] = {
    // Heltec WiFi LoRa 32 V2, TTGO LoRa32 V1 - 가장 일반적
    {"Heltec/TTGO LoRa32 V1/V2", 5, 19, 27, 18, 14, 26, false},
    // TTGO T-Beam (배터리 내장 버전)
    {"TTGO T-Beam",              5, 19, 27, 18, 23, 26, false},
    // Heltec WiFi LoRa 32 V3 - SX1262 (호환 미지원, 더미 엔트리)
    // {"Heltec v3 (SX1262)",     9, 10, 11,  8, 12, 13, true},  // skipped
    // Dragino Lora32 / 기본 SPI
    {"Dragino/Default",          18, 19, 23, 5, 14, 26, false},
    // ESP32 default SPI: SCK=18, MISO=19, MOSI=23 - CS/RST/IRQ 변경
    {"Generic SPI 5/14/26",     18, 19, 23, 5, 14, 26, false},
    {"Generic SPI 5/14/33",     18, 19, 23, 5, 14, 33, false},
};
#define NUM_PINOUTS (sizeof(kPinouts) / sizeof(kPinouts[0]))

// 상태
static bool g_lora_detected = false;
static lora_pinout_t g_active_pinout;
static uint32_t g_packets_rx = 0;
static uint32_t g_packets_tx = 0;
static uint32_t g_drops_too_large = 0;
static uint32_t g_drops_parse_fail = 0;
static uint32_t g_frequency = LORA_FREQ_EU_868;

bool lora_relay_begin(uint32_t frequency_hz) {
    g_frequency = frequency_hz;

    LOGI("lora: scanning for SX127x module (%u pinout candidates)...",
         (unsigned)NUM_PINOUTS);

    for (size_t i = 0; i < NUM_PINOUTS; ++i) {
        const lora_pinout_t* p = &kPinouts[i];
        LOGD("lora: trying %s (sck=%d miso=%d mosi=%d cs=%d rst=%d irq=%d)",
             p->name, p->sck, p->miso, p->mosi, p->cs, p->rst, p->irq);

        // SPI 핀 설정
        if (p->uses_spi_begin) {
            SPI.begin(p->sck, p->miso, p->mosi, p->cs);
        } else {
            SPI.begin(p->sck, p->miso, p->mosi, p->cs);
        }

        LoRa.setPins(p->cs, p->rst, p->irq);

        // LoRa 초기화 (1초 타임아웃)
        if (LoRa.begin(g_frequency) != 1) {
            LOGD("lora: LoRa.begin failed for %s", p->name);
            SPI.end();
            continue;
        }

        // 칩 버전 확인 (SX1276 = 0x12, SX1272 = 0x22)
        uint8_t version = LoRa.readRegister(SX127X_REG_VERSION);
        if (version != SX1276_VERSION_ID && version != SX1272_VERSION_ID) {
            LOGD("lora: %s version 0x%02x not SX127x", p->name, version);
            LoRa.end();
            SPI.end();
            continue;
        }

        // 설정 적용
        LoRa.setTxPower(LORA_TX_POWER_DBM);
        LoRa.setSpreadingFactor(LORA_SPREADING_FACTOR);
        LoRa.setSignalBandwidth(LORA_BANDWIDTH_HZ);
        LoRa.setCodingRate4(LORA_CODING_RATE);
        LoRa.setSyncWord(LORA_SYNC_WORD);
        LoRa.enableCrc();
        LoRa.setPreambleLength(LORA_PREAMBLE_LEN);

        // 수신 콜백 등록
        LoRa.onReceive(lora_relay_handle_receive);
        LoRa.receive();   // receive mode 진입

        // 큰 패킷 분할/재조립 모듈 초기화
        splitter_init();

        g_lora_detected = true;
        g_active_pinout = *p;
        LOGI("lora: [OK] %s detected (SX127%s, version=0x%02x)",
             p->name,
             version == SX1276_VERSION_ID ? "6" : "2",
             version);
        LOGI("lora: freq=%.1fMHz bw=%ukHz SF=%d CR=4/%d pwr=%ddBm sync=0x%02x",
             g_frequency / 1e6,
             LORA_BANDWIDTH_HZ / 1000,
             LORA_SPREADING_FACTOR,
             LORA_CODING_RATE,
             LORA_TX_POWER_DBM,
             LORA_SYNC_WORD);
        return true;
    }

    LOGW("lora: no SX127x module detected, LoRa bridge disabled");
    g_lora_detected = false;
    return false;
}

bool lora_relay_is_detected(void) {
    return g_lora_detected;
}

const char* lora_relay_get_module_name(void) {
    return g_lora_detected ? g_active_pinout.name : "(none)";
}

// 분할 송신용 콜백 데이터
typedef struct {
    uint32_t chunks_sent;
    uint32_t bytes_sent;
    uint32_t failures;
} split_send_ctx_t;

// 청크 단위 TX 콜백 - splitter_send_chunks가 호출
// v23: TX 실패 시 1회 재시도 (LoRa 일시적 충돌 대응)
static bool chunk_tx_callback(const uint8_t* chunk, size_t len, void* user_data) {
    (void)user_data;
    for (int attempt = 0; attempt < 2; attempt++) {  // 최대 2회 시도
        LoRa.beginPacket();
        LoRa.write(chunk, len);
        int rc = LoRa.endPacket();
        if (rc == 1) {
            // 수신 모드로 복귀 (LoRa는 TX 후 자동 복귀 안 됨)
            LoRa.receive();
            return true;
        }
        if (attempt == 0) {
            LOGW("lora: chunk TX fail, retrying (len=%u)", (unsigned)len);
            delay(100);  // 재시도 전 짧은 대기
        }
    }
    LOGW("lora: chunk TX fail after retry (len=%u)", (unsigned)len);
    return false;
}

bool lora_relay_broadcast(const uint8_t* data, size_t len) {
    if (!g_lora_detected) return false;
    if (!data || len == 0) return false;

    // 큰 패킷 -> 분할 송신 (packet_splitter 사용)
    if (len > LORA_MAX_FRAME_LEN) {
        LOGD("lora: packet %u > frame %u, splitting",
             (unsigned)len, LORA_MAX_FRAME_LEN);
        bool ok = splitter_send_chunks(data, len, chunk_tx_callback, nullptr);
        if (ok) {
            g_packets_tx++;
            // 각 청크를 단일 TX로 카운트 (분할 전체를 1개 패킷으로)
            LOGI("lora: split TX complete (%u bytes -> chunks)", (unsigned)len);
        } else {
            g_drops_too_large++;
            LOGW("lora: split TX failed");
        }
        return ok;
    }

    // 작은 패킷 -> 그대로 송신
    LoRa.beginPacket();
    LoRa.write(data, len);
    int rc = LoRa.endPacket();
    if (rc != 1) {
        LOGW("lora: endPacket failed");
        return false;
    }
    g_packets_tx++;
    LoRa.receive();   // 수신 모드로 복귀
    LOGD("lora: TX %u bytes (total=%u)", (unsigned)len, g_packets_tx);
    return true;
}

void lora_relay_handle_receive(int packet_size) {
    if (packet_size <= 0) return;
    if (packet_size > LORA_MAX_FRAME_LEN) {
        // 너무 큰 패킷 - 읽어서 버림
        while (LoRa.available()) LoRa.read();
        g_drops_parse_fail++;
        return;
    }

    // 패킷 데이터 읽기
    static uint8_t buf[BITCHAT_MAX_PKT_LEN];
    if ((size_t)packet_size > sizeof(buf)) {
        while (LoRa.available()) LoRa.read();
        return;
    }
    int read = 0;
    while (LoRa.available() && read < packet_size) {
        buf[read++] = LoRa.read();
    }
    if (read != packet_size) {
        g_drops_parse_fail++;
        return;
    }

    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();
    g_packets_rx++;

    LOGD("lora: RX %d bytes rssi=%d snr=%.1f (total=%u)",
         read, rssi, snr, g_packets_rx);

    // 청크(0xCF)인지 먼저 체크 -> 재조립
    if (splitter_is_chunk(buf, read)) {
        static uint8_t reassembled[BITCHAT_MAX_PKT_LEN];
        size_t reassembled_len = 0;
        if (splitter_receive_chunk(buf, read, reassembled, &reassembled_len)) {
            // 모든 청크 도착 -> 원본 패킷을 BLE 큐로 enqueue
            LOGI("lora: reassembled packet %u bytes -> BLE queue",
                 (unsigned)reassembled_len);
            g_relay.enqueue_packet(reassembled, reassembled_len,
                                    BC_SRC_CLIENT_NOTIFY, 0xFFFD);
        }
        // 아직 모자란 청크는 splitter 내부 버퍼에 저장됨
        return;
    }

    // 일반 bitchat 패킷(0xBC) -> 그대로 BLE 큐에 enqueue
    g_relay.enqueue_packet(buf, (size_t)read, BC_SRC_CLIENT_NOTIFY, 0xFFFD);
}

void lora_relay_debug_dump(void) {
    if (!g_lora_detected) {
        LOGI("lora: not detected");
        return;
    }
    LOGI("=== lora dump ===");
    LOGI("module:  %s", g_active_pinout.name);
    LOGI("pins:    sck=%d miso=%d mosi=%d cs=%d rst=%d irq=%d",
         g_active_pinout.sck, g_active_pinout.miso, g_active_pinout.mosi,
         g_active_pinout.cs, g_active_pinout.rst, g_active_pinout.irq);
    LOGI("freq:    %.1f MHz", g_frequency / 1e6);
    LOGI("stats:   rx=%u tx=%u drop_large=%u drop_parse=%u",
         g_packets_rx, g_packets_tx, g_drops_too_large, g_drops_parse_fail);
    // 큰 패킷 분할/재조립 상태
    splitter_debug_dump();
}
