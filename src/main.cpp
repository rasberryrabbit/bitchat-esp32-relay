/*
 * main.cpp - bitchat ESP32 relay firmware entry point
 *
 * 한국어:
 *   - setup(): 시리얼 초기화 -> crypto/storage 초기화 -> BLE relay 시작
 *   - loop(): BLE relay 메인 루프 + 주기적 상태 출력
 *
 * 3개 빌드 환경 모두 이 파일을 진입점으로 사용:
 *   - PlatformIO (Arduino framework): 이 파일을 src/에서 빌드
 *   - Arduino IDE: src/main.cpp를 sketch 폴더에 .ino로 복사 후 빌드
 *     (단, .ino로 저장 시 파일명과 폴더명 일치 필요)
 *   - ESP-IDF: main/main.cpp로 복사 + CMakeLists.txt로 컴포넌트 빌드
 */

#include <Arduino.h>
#include "log.h"
#include "crypto.h"
#include "storage.h"
#include "ble_relay.h"
#include "bitchat_protocol.h"
#include "power.h"
#include "retry_queue.h"
#include "blacklist.h"
#include "lora_relay.h"
#include "wifi_bridge.h"
#include "peer_keys.h"   // senderID → Ed25519 pk 매핑 (서명 검증용)
#include "packet_splitter.h"   // 큰 패킷 분할/재조립 - LoRa 메시 전용
#include "msg_cache.h"          // store-and-forward 메시지 캐시

bitchat_storage_t g_storage;   // extern-able for wifi_bridge.cpp
static power_config_t   g_power;
static uint32_t g_last_status_ms = 0;

// ============================================================
// Serial 명령 인터페이스 - 블랙리스트 실시간 관리
// ============================================================
//
// 지원 명령 (Serial Monitor에서 입력):
//   bl                       - 현재 블랙리스트 상태 출력
//   bl on                    - 블랙리스트 활성화
//   bl off                   - 블랙리스트 비활성화 (모든 체크 skip)
//   bl add <hex>             - 추가 (32B pk=64hex / 6B ble=12hex 자동 감지)
//   bl rm <hex>              - 제거
//   bl clear                 - 전체 초기화
//   bl stats                 - 카운트 출력
//   help                     - 도움말

static char g_cmd_buffer[128];
static size_t g_cmd_len = 0;

static void print_help(void) {
    Serial.println(F("\r\n=== Serial Commands ==="));
    Serial.println(F("bl              - list blacklist"));
    Serial.println(F("bl on/off       - enable/disable filtering"));
    Serial.println(F("bl add <hex>    - add (32B pk=64hex / 6B ble=12hex)"));
    Serial.println(F("bl rm <hex>      - remove"));
    Serial.println(F("bl clear        - clear all"));
    Serial.println(F("bl stats        - counts only"));
    Serial.println(F("wifi            - WiFi+MQTT status"));
    Serial.println(F("wifi on [s p]   - connect (NVS or args; SSID spaces OK)"));
    Serial.println(F("wifi off        - disconnect"));
    Serial.println(F("wifi set <s> <p>- save creds (SSID spaces OK, pass=last word)"));
    Serial.println(F("wifi stat       - stats only"));
    Serial.println(F("help            - this help"));
    Serial.println(F("=======================\r\n"));
}

static void handle_blacklist_command(char* args) {
    if (!args || *args == 0) {
        // 인자 없으면 dump
        blacklist_debug_dump();
        return;
    }

    // 첫 단어 파싱
    char* cmd = args;
    char* arg = strchr(args, ' ');
    if (arg) { *arg++ = 0; }

    if (strcmp(cmd, "on") == 0) {
        blacklist_set_enabled(true);
    } else if (strcmp(cmd, "off") == 0) {
        blacklist_set_enabled(false);
    } else if (strcmp(cmd, "clear") == 0) {
        blacklist_clear_all();
    } else if (strcmp(cmd, "stats") == 0) {
        LOGI("blacklist stats: pk=%u ble=%u enabled=%d",
             (unsigned)blacklist_get_pk_count(),
             (unsigned)blacklist_get_ble_count(),
             blacklist_is_enabled());
    } else if (strcmp(cmd, "add") == 0 && arg) {
        uint8_t bytes[64];
        size_t blen = blacklist_hex_to_bytes(arg, bytes, sizeof(bytes));
        if (blen == 32) {
            blacklist_add_pk(bytes);
        } else if (blen == 6) {
            blacklist_add_ble(bytes);
        } else {
            LOGE("invalid hex length: %u (need 64 for pk or 12 for ble)",
                 (unsigned)blen);
        }
    } else if (strcmp(cmd, "rm") == 0 && arg) {
        uint8_t bytes[64];
        size_t blen = blacklist_hex_to_bytes(arg, bytes, sizeof(bytes));
        if (blen == 32) {
            blacklist_remove_pk(bytes);
        } else if (blen == 6) {
            blacklist_remove_ble(bytes);
        } else {
            LOGE("invalid hex length: %u", (unsigned)blen);
        }
    } else {
        LOGE("unknown blacklist subcommand: %s", cmd);
    }
}

// ============================================================
// WiFi bridge Serial 명령 처리
//
// 지원 명령:
//   wifi                  - 현재 상태 출력
//   wifi on               - WiFi+MQTT 연결 시도 (NVS 또는 컴파일 타임 값 사용)
//   wifi on <ssid> <pass> - 지정한 SSID/비번으로 연결
//   wifi off              - 연결 해제
//   wifi stat             - 통계만 출력
//   wifi set <ssid> <pass>- NVS에 영속 저장 (재부팅 후 'wifi on'에서 자동 사용)
//   wifi broker <host>    - MQTT broker 호스트 변경 (RAM만, 기본 test.mosquitto.org)
//
// ============================================================
static void handle_wifi_command(char* args) {
    if (!args || *args == 0) {
        wifi_bridge_debug_dump();
        return;
    }
    char* cmd = args;
    char* arg = strchr(args, ' ');
    if (arg) { *arg++ = 0; }

    if (strcmp(cmd, "on") == 0) {
        if (wifi_bridge_is_connected()) {
            LOGW("wifi already connected, use 'wifi off' first");
            return;
        }
        if (arg && *arg) {
            // wifi on <ssid> <pass> - SSID에 공백 가능 (마지막 공백으로 분할)
            // 예: wifi on My Home WiFi mypass123
            //     → ssid="My Home WiFi", pass="mypass123"
            char* pass = strrchr(arg, ' ');
            if (pass) { *pass++ = 0; }
            if (!pass || !*pass) {
                LOGE("usage: wifi on <ssid> <pass>  (SSID spaces OK, pass=last word)");
                return;
            }
            wifi_bridge_begin_connection(arg, pass);
        } else {
            // NVS 또는 컴파일 타임 매크로에서 조회
            wifi_bridge_begin_connection(nullptr, nullptr);
        }
    } else if (strcmp(cmd, "off") == 0) {
        wifi_bridge_disconnect();
    } else if (strcmp(cmd, "stat") == 0) {
        wifi_bridge_debug_dump();
    } else if (strcmp(cmd, "set") == 0 && arg) {
        // wifi set <ssid> <pass> - SSID에 공백 가능 (마지막 공백으로 분할)
        // 예: wifi set My Home WiFi mypass123
        //     → ssid="My Home WiFi", pass="mypass123" (NVS에 저장)
        char* pass = strrchr(arg, ' ');
        if (pass) { *pass++ = 0; }
        if (!pass || !*pass) {
            LOGE("usage: wifi set <ssid> <pass>  (SSID spaces OK, pass=last word)");
            return;
        }
        wifi_bridge_save_credentials(arg, pass);
    } else if (strcmp(cmd, "broker") == 0 && arg) {
        extern void wifi_bridge_set_broker(const char* host);
        // 선언 안 했으므로 inline으로는 안 됨. 직접 구조 변경 필요.
        LOGW("'wifi broker' not supported yet, edit BC_MQTT_BROKER macro");
    } else {
        LOGE("unknown wifi subcommand: %s", cmd);
    }
}

static void process_serial_input(void) {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (g_cmd_len > 0) {
                g_cmd_buffer[g_cmd_len] = 0;
                Serial.print("\r\n> ");
                Serial.println(g_cmd_buffer);

                // 명령 파싱
                char* cmd = g_cmd_buffer;
                char* args = strchr(cmd, ' ');
                if (args) { *args++ = 0; }

                if (strcmp(cmd, "bl") == 0) {
                    handle_blacklist_command(args);
                } else if (strcmp(cmd, "wifi") == 0) {
                    handle_wifi_command(args);
                } else if (strcmp(cmd, "help") == 0) {
                    print_help();
                } else if (strcmp(cmd, "") != 0) {
                    LOGE("unknown command: %s (type 'help')", cmd);
                }
                g_cmd_len = 0;
            }
        } else if (g_cmd_len < sizeof(g_cmd_buffer) - 1) {
            g_cmd_buffer[g_cmd_len++] = c;
        }
    }
}

void setup() {
    // 시리얼 초기화 - USB serial (디버깅/최초 설정용)
    // 배터리 모드에서도 USB가 연결된 동안은 로그 출력
    Serial.begin(115200);
    delay(100);
    Serial.println("\r\n=============================");
    Serial.println(" bitchat ESP32 relay firmware");
    Serial.println(" build: " __DATE__ " " __TIME__);
#if defined(BC_BOARD_IS_C3)
    Serial.println(" target: ESP32-C3 (RISC-V, BLE 5.0)");
#elif defined(BC_BOARD_IS_S3)
    Serial.println(" target: ESP32-S3 (Xtensa LX7, BLE 5.0 + USB-OTG)");
#elif defined(BOARD_VARIANT_ESP32)
    Serial.println(" target: ESP32 classic (Xtensa LX6, BLE 4.2)");
#else
    Serial.println(" target: ESP32 (auto-detected)");
#endif
    Serial.print   (" max-conn: in=");
    Serial.print(BC_MAX_IN_CONN);
    Serial.print   (" out=");
    Serial.print(BC_MAX_OUT_CONN);
    Serial.print   (" queue=");
    Serial.println(BC_PKT_QUEUE_LEN);
#if defined(BC_POWER_BATTERY)
    Serial.println(" power: BATTERY + light/deep sleep");
#elif defined(BC_POWER_USB_ALWAYS)
    Serial.println(" power: USB always-on");
#else
    Serial.println(" power: USB always-on (default)");
#endif
    Serial.println("=============================\r\n");

    // crypto (mbedtls) 준비
    crypto_init();

    // 전원 관리 초기화 (배터리 모드 시 ESP-PM 라이트 슬립 활성화)
    power_get_default_config(&g_power);
    if (!power_init(&g_power)) {
        LOGW("power_init failed, continuing without power management");
    }

    // NVS에서 자체 키 복원 또는 새로 생성
    if (!storage_init(&g_storage)) {
        LOGE("storage_init failed, halting");
        while (true) { delay(1000); }
    }
    storage_debug_dump(&g_storage);

    // BLE relay 시작
    if (!g_relay.begin(&g_storage)) {
        LOGE("BLE relay init failed, halting");
        while (true) { delay(1000); }
    }
    LOGI("=== bitchat relay node ready ===");

    // LoRa 모듈 자동 감지 (있으면 BLE↔LoRa 브릿지 자동 활성화)
    if (lora_relay_begin(LORA_FREQ_KR_921)) {
        LOGI("LoRa bridge ENABLED: %s",
             lora_relay_get_module_name());
    } else {
        LOGI("LoRa module not detected - BLE-only mode");
    }

    // WiFi + MQTT 브릿지 초기화 (실제 연결은 Serial 'wifi on' 명령 필요)
    // 자동 연결하지 않음 - 사용자가 WiFi를 명시적으로 켤 때만 동작 (BLE 우선)
    if (wifi_bridge_init()) {
        LOGI("WiFi bridge ready (use 'wifi on' to connect, 'wifi set <s> <p>' to save creds)");
    } else {
        LOGW("WiFi bridge init failed");
    }
}

void loop() {
    // Serial 명령 처리 - 블랙리스트 + WiFi 실시간 관리
    process_serial_input();

    // BLE relay 메인 루프
    g_relay.loop();

    // WiFi + MQTT 브릿지 - keep-alive + 수신 패킷 처리
    // (활성화된 경우에만 동작, BLE보다 낮은 우선순위)
    if (wifi_bridge_is_enabled()) {
        wifi_bridge_loop();
    }

    // 큰 패킷 분할/재조립 - 30초 타임아웃된 미완료 슬롯 정리 (1초마다)
    static uint32_t last_splitter_cleanup = 0;
    if (millis() - last_splitter_cleanup > 1000) {
        last_splitter_cleanup = millis();
        splitter_cleanup_timeouts();
        msg_cache_cleanup();   // 12시간 경과 캐시 항목 제거
    }

    // 전원 관리 - 유휴 시 라이트 슬립 + 상태 LED + 배터리 모니터링
    power_handle_idle();

    // 5초마다 상태 요약
    if (millis() - g_last_status_ms > 5000) {
        g_last_status_ms = millis();
        LOGI("--- status: in=%d out=%d ctr=%llu peers=%u ---",
             g_relay.incoming_count(),
             g_relay.outgoing_count(),
             (unsigned long long)g_storage.self_counter,
             g_storage.peer_count);
        // 배터리 전압을 상태 출력마다 표시
        if (g_power.battery_adc_pin != 0xFF) {
            power_battery_debug();
        }
        // ACK 재전송 큐 통계
        retry_stats_t rs;
        retry_get_stats(&rs);
        LOGI("--- retry: pending=%u retrans=%u p_ack=%u e_ack=%u dropped=%u ---",
             rs.pending_count, rs.total_retransmits,
             rs.total_passive_acks, rs.total_explicit_acks, rs.total_dropped);
        // 블랙리스트 카운트 (상세는 bl stats 또는 bl 명령으로 확인)
        LOGI("--- blacklist: pk=%u ble=%u enabled=%d ---",
             (unsigned)blacklist_get_pk_count(),
             (unsigned)blacklist_get_ble_count(),
             blacklist_is_enabled());
        // peer_keys 매핑 (서명 검증용)
        LOGI("--- peer_keys: %u mappings ---",
             (unsigned)peer_keys_count());
        // store-and-forward 캐시
        LOGI("--- msg_cache: %u cached ---",
             (unsigned)msg_cache_count());
        // LoRa 모듈 상태 (감지된 경우)
        if (lora_relay_is_detected()) {
            lora_relay_debug_dump();
        }
        // WiFi 브릿지 상태 (활성화된 경우만)
        if (wifi_bridge_is_enabled()) {
            wifi_bridge_debug_dump();
        }
    }

    // CPU 양보 - 전원 관리에서 delay(1) 호출로 자동 라이트 슬립 트리거됨
    // power_handle_idle()이 delay(1)을 호출하므로 별도 delay 불필요
}
