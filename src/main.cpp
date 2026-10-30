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
#include <SPIFFS.h>
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
#include "msg_store.h"          // SPIFFS overflow backend for msg_cache
#include "node_config.h"        // SPIFFS-based node profile + wifi/mqtt config

bitchat_storage_t g_storage;   // extern-able for wifi_bridge.cpp
static power_config_t   g_power;
static uint32_t g_last_status_ms = 0;

// v17: 데이터 덤프 런타임 플래그 (RAM only, NVS 저장 안 함, 기본 OFF)
//   log.h의 LOGDUMP 매크로와 log_hexdump()가 이 플래그 참조.
//   Serial 명령 'dump on'으로 활성화, 'dump off'로 비활성화.
//   재부팅 시 항상 OFF (운용 중 임시 디버깅용, 성능 보호).
bool g_log_dump_enabled = false;

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
    Serial.println(F("bat             - battery voltage + ignore status"));
    Serial.println(F("bat ignore on   - FORCE BLE keep-alive at V_CRIT (NVS persisted)"));
    Serial.println(F("bat ignore off  - normal V_CRIT behavior (NVS persisted)"));
    Serial.println(F("bat read        - immediate ADC read"));
    Serial.println(F("cache           - cache status (RAM + SPIFFS overflow)"));
    Serial.println(F("cache dump      - list all cached messages"));
    Serial.println(F("cache clear     - wipe SPIFFS overflow store"));
    Serial.println(F("cache spiffs    - show SPIFFS cache enable flag"));
    Serial.println(F("cache spiffs on - enable SPIFFS overflow cache (default, NVS persisted)"));
    Serial.println(F("cache spiffs off- disable SPIFFS overflow (drop only, no flash writes)"));
    Serial.println(F("sleep           - power management status"));
    Serial.println(F("sleep deep on   - enable deep sleep after 5min idle (NVS persisted)"));
    Serial.println(F("sleep deep off  - disable deep sleep (default, light sleep only)"));
    Serial.println(F("txpower         - show BLE TX power"));
    Serial.println(F("txpower <N>     - set BLE TX power in dBm (-12..9, NVS persisted)"));
    Serial.println(F("dump            - show packet dump status (default OFF)"));
    Serial.println(F("dump on         - enable RX write + hex dump (RAM only, not persisted)"));
    Serial.println(F("dump off        - disable dump (performance-safe)"));
    Serial.println(F("peer            - peer_keys mappings (senderID → Ed25519 pk)"));
    Serial.println(F("peer list       - list all peer keys"));
    Serial.println(F("peer clear      - clear all peer keys"));
    Serial.println(F("node             - node profile (nickname)"));
    Serial.println(F("node nick <name>- set node nickname (SPIFFS persisted)"));
    Serial.println(F("node nick clear  - remove nickname"));
    Serial.println(F("wifi broker <h> [p] - MQTT broker + port (SPIFFS persisted)"));
    Serial.println(F("wifi prefix <p>      - MQTT topic prefix (SPIFFS persisted)"));
    Serial.println(F("wifi tls on          - enable mTLS MQTT (port 8883, certs required)"));
    Serial.println(F("wifi tls off         - disable TLS (plain MQTT port 1883, default)"));
    Serial.println(F("cert status          - show cert files status (CA/client/key)"));
    Serial.println(F("cert upload <ca|client|key> - paste PEM (end with 'EOF' on its own line)"));
    Serial.println(F("cert clear           - delete all cert PEM files"));
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
        // wifi broker <host> [port] - MQTT broker 변경 (SPIFFS 영속화)
        char* host = arg;
        char* port_str = strchr(arg, ' ');
        uint16_t port = 1883;
        if (port_str) {
            *port_str++ = 0;
            port = (uint16_t)atoi(port_str);
            if (port == 0) port = 1883;
        }
        if (!wifi_bridge_set_broker(host, port)) {
            LOGE("wifi: failed to set broker (SPIFFS write error)");
        }
    } else if (strcmp(cmd, "prefix") == 0 && arg) {
        // wifi prefix <prefix> - MQTT topic prefix 변경 (SPIFFS 영속화)
        if (!wifi_bridge_set_topic_prefix(arg)) {
            LOGE("wifi: failed to set prefix (SPIFFS write error)");
        }
    } else if (strcmp(cmd, "tls") == 0) {
        // wifi tls [on|off|status]
        if (!arg || *arg == 0 || strcmp(arg, "status") == 0) {
            int mask = wifi_bridge_check_certs();
            LOGI("wifi: tls=%s (port=%u)",
                 wifi_bridge_get_tls_enabled() ? "ON" : "OFF",
                 wifi_bridge_get_port());
            LOGI("  certs: CA=%s, client_cert=%s, client_key=%s (mask=0x%x)",
                 (mask & 1) ? "OK" : "MISSING",
                 (mask & 2) ? "OK" : "MISSING",
                 (mask & 4) ? "OK" : "MISSING", mask);
        } else if (strcmp(arg, "on") == 0 || strcmp(arg, "enable") == 0) {
            wifi_bridge_set_tls_enabled(true);
        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "disable") == 0) {
            wifi_bridge_set_tls_enabled(false);
        } else {
            LOGE("usage: wifi tls [on|off|status]");
        }
    } else {
        LOGE("unknown wifi subcommand: %s", cmd);
    }
}

// ============================================================
// 노드 프로필 Serial 명령 처리
//
// 지원 명령:
//   node                - 현재 프로필 출력 (nickname 등)
//   node nick <name>    - nickname 설정 (SPIFFS /node.conf에 영속화)
//   node nick clear     - nickname 제거
//
// nickname 용도:
//   - BLE 광고 이름에 반영 (bitchat-<nick>) - reboot 후 적용
//   - 상태 출력 시 자기 식별용
//   - 향후 메시지 헤더에 포함 가능 (TODO)
// ============================================================
static void handle_node_command(char* args) {
    if (!args || *args == 0) {
        // 현재 프로필 출력
        const char* nick = node_config_get_nickname();
        char id[BITCHAT_ID_STR_LEN];
        bitchat_id_from_pk(g_storage.self_kp.public_key, id);
        LOGI("=== node profile ===");
        LOGI("self_id=%s", id);
        LOGI("nickname=%s", nick[0] ? nick : "(not set)");
        LOGI("ble_adv_name=%s", nick[0] ? "bitchat-<nick>" : "bitchat");
        LOGI("(file: /node.conf)");
        return;
    }
    char* cmd = args;
    char* arg = strchr(args, ' ');
    if (arg) { *arg++ = 0; }

    if (strcmp(cmd, "nick") == 0 || strcmp(cmd, "nickname") == 0) {
        if (!arg || *arg == 0) {
            LOGE("usage: node nick <name>  or  node nick clear");
            return;
        }
        if (strcmp(arg, "clear") == 0 || strcmp(arg, "off") == 0 ||
            strcmp(arg, "none") == 0) {
            if (node_config_set_nickname("")) {
                LOGI("nickname cleared (SPIFFS /node.conf updated)");
                LOGW("reboot required to update BLE adv name");
            } else {
                LOGE("failed to clear nickname");
            }
        } else {
            if (node_config_set_nickname(arg)) {
                LOGI("nickname='%s' saved to /node.conf", arg);
                LOGW("reboot required to update BLE adv name");
            } else {
                LOGE("failed to set nickname");
            }
        }
    } else {
        LOGE("unknown node subcommand: %s (try: nick <name>)", cmd);
    }
}

// ============================================================
// 배터리 Serial 명령 처리
//
// 지원 명령:
//   bat                   - 현재 배터리 전압 + ignore 플래그 상태 출력
//   bat status            - 위와 동일 (alias)
//   bat ignore on         - 배터리 임계치 도달해도 BLE 중지하지 않음 (NVS 영속화)
//   bat ignore off        - 정상 동작으로 복귀 (NVS 영속화)
//   bat ignore            - 현재 ignore 상태만 출력
//   bat read              - ADC에서 전압 1회 즉시 읽기
//
// 사용 예:
//   - USB 상시 전원 환경에서 배터리 분배 핀이 있어 CRIT 경고 뜨는 경우
//     `bat ignore on` → 재부팅 후에도 BLE 유지
//   - Heltec V2에서 GPIO 13(ADC2)이 WiFi 켜지면 garbage 값 읽어 CRIT 오탐지
//     `bat ignore on`으로 회피 가능
// ============================================================
static void handle_battery_command(char* args) {
    if (!args || *args == 0 || strcmp(args, "status") == 0) {
        // 현재 상태 출력
        float v = power_read_battery_voltage();
        bool ignored = power_is_battery_ignored();
        if (v < 0) {
            LOGI("battery: N/A (ADC disabled or not initialized)");
        } else {
            LOGI("battery: %.2fV %s", v, ignored ? "(IGNORED)" : "");
        }
        LOGI("battery_ignore: %s (NVS-persisted, survives reboot)",
             ignored ? "ON" : "OFF");
        return;
    }

    char* cmd = args;
    char* arg = strchr(args, ' ');
    if (arg) { *arg++ = 0; }

    if (strcmp(cmd, "ignore") == 0) {
        if (!arg || *arg == 0) {
            // bat ignore - 상태만 출력
            LOGI("battery_ignore: %s",
                 power_is_battery_ignored() ? "ON" : "OFF");
            return;
        }
        if (strcmp(arg, "on") == 0) {
            if (storage_set_battery_ignore(true)) {
                power_reload_battery_ignore();  // power.cpp 캐시 무효화
                LOGI("battery_ignore=ON (persisted, BLE will keep-alive at V_CRIT)");
            } else {
                LOGE("failed to set battery_ignore=ON (NVS error)");
            }
        } else if (strcmp(arg, "off") == 0) {
            if (storage_set_battery_ignore(false)) {
                power_reload_battery_ignore();
                LOGI("battery_ignore=OFF (persisted, normal V_CRIT behavior)");
            } else {
                LOGE("failed to set battery_ignore=OFF (NVS error)");
            }
        } else {
            LOGE("usage: bat ignore [on|off]");
        }
    } else if (strcmp(cmd, "read") == 0) {
        // 즉시 ADC 읽기
        float v = power_read_battery_voltage();
        if (v < 0) {
            LOGI("battery: ADC not available");
        } else {
            LOGI("battery read: %.3fV (raw)", v);
        }
    } else {
        LOGE("unknown battery subcommand: %s (try: status, ignore on|off, read)", cmd);
    }
}

// ============================================================
// 절전 모드 Serial 명령 처리
//
// 지원 명령:
//   sleep             - 현재 절전 모드 상태 출력 (battery_ignore, spiffs_cache, deep_sleep)
//   sleep deep on     - 5분 idle 시 딥슬립 허용 (NVS 영속화)
//   sleep deep off    - 딥슬립 비활성화, light sleep만 사용 (기본값, NVS 영속화)
//
// ESP32 딥슬립 동작 (주의):
//   - wake 시 무조건 reset (RAM 초기화, setup()부터 재실행)
//   - BLE 컨트롤러 끊김 → 모든 peer 연결 해제
//   - RAM 상태 (peer_keys, msg_cache, retry_queue) 손실
//   - ~1초 재부팅 시간 동안 메시지 유실 가능
//
// 기본값 OFF인 이유:
//   - BLE 메시 릴레이 노드는 RAM 상태 + BLE continuity가 절전보다 중요
//   - auto light sleep (esp_pm)이 CPU만 절전하므로 충분한 효과
//   - 딥슬립은 배터리 극단 부족 / 장기 미사용 시에만 ON
// ============================================================
static void handle_sleep_command(char* args) {
    if (!args || *args == 0 || strcmp(args, "status") == 0) {
        LOGI("=== power management ===");
        LOGI("battery_ignore=%s (BLE keep-alive at V_CRIT)",
             storage_get_battery_ignore() ? "ON" : "OFF");
        LOGI("spiffs_cache: user=%s, runtime=%s, effective=%s (store-and-forward overflow)",
             storage_get_spiffs_cache_enabled() ? "ON" : "OFF",
             storage_get_spiffs_runtime_disabled() ? "DISABLED(battery)" : "OK",
             storage_get_spiffs_cache_effective() ? "ON" : "OFF");
        LOGI("deep_sleep_enabled=%s (5min idle → deep sleep + 60s wake reset)",
             storage_get_deep_sleep_enabled() ? "ON" : "OFF (default, light sleep only)");
        return;
    }
    char* cmd = args;
    char* arg = strchr(args, ' ');
    if (arg) { *arg++ = 0; }

    if (strcmp(cmd, "deep") == 0) {
        if (!arg || *arg == 0) {
            LOGI("deep_sleep: %s",
                 storage_get_deep_sleep_enabled() ? "ON" : "OFF");
            return;
        }
        if (strcmp(arg, "on") == 0 || strcmp(arg, "enable") == 0) {
            if (storage_set_deep_sleep_enabled(true)) {
                LOGI("deep_sleep=ON (persisted, 5min idle → reset + 60s sleep)");
                LOGW("주의: 딥슬립 wake 시 BLE 연결 끊김 + RAM 상태 손실");
            } else {
                LOGE("failed to set deep_sleep=ON (NVS error)");
            }
        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "disable") == 0) {
            if (storage_set_deep_sleep_enabled(false)) {
                LOGI("deep_sleep=OFF (persisted, light sleep only - BLE continuity 유지)");
            } else {
                LOGE("failed to set deep_sleep=OFF (NVS error)");
            }
        } else {
            LOGE("usage: sleep deep [on|off]");
        }
    } else {
        LOGE("unknown sleep subcommand: %s (try: deep [on|off])", cmd);
    }
}

// ============================================================
// peer_keys Serial 명령 처리
//
// 지원 명령:
//   peer              - 현재 peer_keys 매핑 상태 출력 (alias of list)
//   peer list         - 모든 매핑 dump (senderID + pk)
//   peer clear        - 전체 삭제 (SPIFFS /peer_keys.bin 제거)
//   peer count        - 개수만 출력
//
// 매핑 추가는 자동: BLE ANNOUNCE 메시지 수신 시 ble_relay.cpp에서 peer_keys_add() 호출
// ============================================================

// ============================================================
// BLE TX power Serial 명령 처리
//
// 지원 명령:
//   txpower           - 현재 TX power 출력
//   txpower <N>       - TX power를 N dBm으로 설정 (NVS 영속화, 즉시 적용)
//                       허용 범위: -12 ~ +9 (ESP32 classic 최대 +9)
//
// 사용 예:
//   txpower 9         - 최대 출력 (기본값, 발견 확률 최우선)
//   txpower 0         - 0 dBm (근거리 + 절전)
//   txpower -3        - -3 dBm (극단적 절전)
// ============================================================
static void handle_txpower_command(char* args) {
    if (!args || *args == 0) {
        int8_t cur = storage_get_tx_power();
        LOGI("txpower: %+ddBm (NVS-persisted, range -12..9)", (int)cur);
        return;
    }
    // strtol로 파싱 (음수 허용)
    char* endp = nullptr;
    long val = strtol(args, &endp, 10);
    if (endp == args || *endp != 0) {
        LOGE("usage: txpower <dBm>  (e.g. txpower 9, txpower 0, txpower -3)");
        return;
    }
    if (val < -12 || val > 9) {
        LOGE("txpower out of range: %ld (allowed -12..9)", val);
        return;
    }
    if (storage_set_tx_power((int8_t)val)) {
        // storage_set_tx_power이 ble_apply_tx_power()를 호출하여 즉시 적용
        LOGI("txpower set to %+ddBm (applied + persisted)", (int)val);
    } else {
        LOGE("failed to set txpower (NVS error)");
    }
}

// ============================================================
// 데이터 덤프 Serial 명령 처리 (v17)
//
// 지원 명령:
//   dump              - 현재 덤프 활성화 상태 출력
//   dump on           - RX write / hex dump 출력 활성화 (RAM only, NVS 저장 안 함)
//   dump off          - 덤프 비활성화 (기본값, 성능 보호)
//
// 성능 영향:
//   - 166B 패킷 hex dump 시 ~73ms Serial 블로킹 (115200 bps)
//   - BLE write 버스트(30~90ms 간격)와 겹치면 RX accumulator 지연 → 패킷 손실 가능
//   - 디버깅 시에만 dump on, 분석 후 반드시 dump off
//
// NVS 저장 안 함 이유:
//   재부팅 후 성능 보호를 위해 항상 OFF로 시작.
//   운용 중 임시 디버깅용으로만 사용.
// ============================================================
static void handle_dump_command(char* args) {
    if (!args || *args == 0) {
        LOGI("dump: %s (RAM-only, NVS not persisted, default OFF)",
             g_log_dump_enabled ? "ON" : "OFF");
        return;
    }
    if (strcmp(args, "on") == 0 || strcmp(args, "enable") == 0) {
        g_log_dump_enabled = true;
        LOGI("dump=ON (RX write + hex dump 출력 활성화, RAM-only, 재부팅 시 OFF)");
        LOGI("주의: 성능 저하 가능. 분석 후 'dump off' 권장");
    } else if (strcmp(args, "off") == 0 || strcmp(args, "disable") == 0) {
        g_log_dump_enabled = false;
        LOGI("dump=OFF (기본값, 성능 보호)");
    } else {
        LOGE("usage: dump [on|off]");
    }
}

static void handle_peer_command(char* args) {
    if (!args || *args == 0 || strcmp(args, "list") == 0 || strcmp(args, "status") == 0) {
        peer_keys_debug_dump();
        return;
    }
    char* cmd = args;
    char* arg = strchr(args, ' ');
    if (arg) { *arg++ = 0; }

    if (strcmp(cmd, "clear") == 0) {
        peer_keys_clear();
        LOGI("peer_keys cleared (SPIFFS /peer_keys.bin removed)");
    } else if (strcmp(cmd, "count") == 0) {
        LOGI("peer_keys count: %u", (unsigned)peer_keys_count());
    } else {
        LOGE("unknown peer subcommand: %s (try: list, clear, count)", cmd);
    }
}

// ============================================================
// 인증서 PEM 파일 업로드 Serial 명령 처리
//
// 지원 명령:
//   cert                  - cert 파일 상태 출력 (CA/client_cert/client_key)
//   cert status           - 위와 동일
//   cert upload <target>  - PEM 내용을 붙여넣기 모드로 업로드
//                            target: ca | client | key
//                            EOF 라인(또는 빈 라인)까지 읽어 SPIFFS에 저장
//   cert clear            - 모든 PEM 파일 삭제 (/broker_ca.pem, /client_cert.pem, /client_key.pem)
//   cert show <target>    - PEM 파일 내용 출력 (디버깅)
//
// PEM 파일 경로:
//   /broker_ca.pem   - 브로커 CA 인증서 (TLS 시 필수)
//   /client_cert.pem - 클라이언트 인증서 (mTLS 시 필수)
//   /client_key.pem  - 클라이언트 개인키 (mTLS 시 필수)
// ============================================================

// 인증서 파일 경로 (wifi_bridge.cpp와 동일)
#define BROKER_CA_FILE    "/broker_ca.pem"
#define CLIENT_CERT_FILE  "/client_cert.pem"
#define CLIENT_KEY_FILE   "/client_key.pem"

// target 문자열 → 파일 경로 변환
static const char* cert_target_to_path(const char* target) {
    if (!target) return nullptr;
    if (strcmp(target, "ca") == 0 || strcmp(target, "broker_ca") == 0) {
        return BROKER_CA_FILE;
    } else if (strcmp(target, "client") == 0 || strcmp(target, "client_cert") == 0 ||
               strcmp(target, "cert") == 0) {
        return CLIENT_CERT_FILE;
    } else if (strcmp(target, "key") == 0 || strcmp(target, "client_key") == 0) {
        return CLIENT_KEY_FILE;
    }
    return nullptr;
}

// PEM 업로드: 'EOF' 라인 또는 빈 라인까지 읽어 파일에 저장
// Serial 입력 버퍼에서 한 번에 한 줄씩 읽음
static bool upload_pem(const char* path) {
    LOGI("cert: paste PEM content for %s", path);
    LOGI("cert: end with a single line containing only 'EOF' (or send empty line)");

    File f = SPIFFS.open(path, "w");
    if (!f) {
        LOGE("cert: open(%s, w) failed", path);
        return false;
    }

    // 임시 버퍼: PEM은 한 줄이 최대 80자 (base64 64자 + header/footer)
    char line[128];
    int line_pos = 0;
    uint32_t start = millis();
    uint32_t timeout_ms = 60000;  // 60초 내에 EOF 안 오면 취소
    size_t total_bytes = 0;
    int total_lines = 0;

    while (millis() - start < timeout_ms) {
        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\r') continue;  // CR 무시
            if (c == '\n') {
                line[line_pos] = 0;
                // EOF 체크
                if (strcmp(line, "EOF") == 0 || (line_pos == 0)) {
                    f.close();
                    LOGI("cert: upload done (%u bytes, %d lines) → %s",
                         (unsigned)total_bytes, total_lines, path);
                    return true;
                }
                // 파일에 라인 저장
                f.println(line);
                total_bytes += line_pos + 2;  // \r\n 가정
                total_lines++;
                // 진행 표시
                if (total_lines % 5 == 0) {
                    Serial.print(".");
                }
                line_pos = 0;
                start = millis();  // 타임아웃 리셋
            } else if (line_pos < (int)sizeof(line) - 1) {
                line[line_pos++] = c;
            }
            // 라인 길이 초과 시 무시 (PEM은 80자 이내)
        }
        // 잠시 대기 (CPU 양보)
        delay(10);
    }

    f.close();
    LOGE("cert: upload timeout (60s no EOF) - partial data discarded");
    SPIFFS.remove(path);  // 부분 업로드된 파일 삭제
    return false;
}

static void handle_cert_command(char* args) {
    if (!args || *args == 0 || strcmp(args, "status") == 0) {
        // 상태 출력
        int mask = wifi_bridge_check_certs();
        LOGI("=== cert status ===");
        LOGI("  /broker_ca.pem   : %s", (mask & 1) ? "OK" : "MISSING");
        LOGI("  /client_cert.pem : %s", (mask & 2) ? "OK" : "MISSING");
        LOGI("  /client_key.pem  : %s", (mask & 4) ? "OK" : "MISSING");
        LOGI("  mask=0x%x (%s mode)", mask,
             mask == 7 ? "mTLS ready" : (mask & 1) ? "CA-only TLS" : "TLS unavailable");
        LOGI("  use_tls=%s (current wifi.conf)", wifi_bridge_get_tls_enabled() ? "ON" : "OFF");
        return;
    }
    char* cmd = args;
    char* arg = strchr(args, ' ');
    if (arg) { *arg++ = 0; }

    if (strcmp(cmd, "upload") == 0) {
        if (!arg || *arg == 0) {
            LOGE("usage: cert upload <ca|client|key>");
            return;
        }
        const char* path = cert_target_to_path(arg);
        if (!path) {
            LOGE("unknown cert target: %s (try: ca, client, key)", arg);
            return;
        }
        if (!upload_pem(path)) {
            LOGE("cert: upload failed for %s", path);
        }
    } else if (strcmp(cmd, "clear") == 0) {
        bool removed_any = false;
        if (SPIFFS.remove(BROKER_CA_FILE))   removed_any = true;
        if (SPIFFS.remove(CLIENT_CERT_FILE)) removed_any = true;
        if (SPIFFS.remove(CLIENT_KEY_FILE))  removed_any = true;
        LOGI("cert: cleared all PEM files (%s)",
             removed_any ? "some removed" : "none existed");
    } else if (strcmp(cmd, "show") == 0) {
        if (!arg || *arg == 0) {
            LOGE("usage: cert show <ca|client|key>");
            return;
        }
        const char* path = cert_target_to_path(arg);
        if (!path) {
            LOGE("unknown cert target: %s", arg);
            return;
        }
        if (!SPIFFS.exists(path)) {
            LOGE("cert: %s does not exist", path);
            return;
        }
        File f = SPIFFS.open(path, "r");
        if (!f) {
            LOGE("cert: open(%s) failed", path);
            return;
        }
        LOGI("=== %s (%u bytes) ===", path, (unsigned)f.size());
        while (f.available()) {
            String line = f.readStringUntil('\n');
            Serial.println(line);
        }
        f.close();
        LOGI("=== end of %s ===", path);
    } else {
        LOGE("unknown cert subcommand: %s (try: status, upload, clear, show)", cmd);
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
                } else if (strcmp(cmd, "bat") == 0) {
                    handle_battery_command(args);
                } else if (strcmp(cmd, "node") == 0) {
                    handle_node_command(args);
                } else if (strcmp(cmd, "sleep") == 0) {
                    handle_sleep_command(args);
                } else if (strcmp(cmd, "txpower") == 0) {
                    handle_txpower_command(args);
                } else if (strcmp(cmd, "dump") == 0) {
                    handle_dump_command(args);
                } else if (strcmp(cmd, "peer") == 0) {
                    handle_peer_command(args);
                } else if (strcmp(cmd, "cert") == 0) {
                    handle_cert_command(args);
                } else if (strcmp(cmd, "cache") == 0) {
                    if (!args || *args == 0 || strcmp(args, "status") == 0) {
                        msg_cache_debug_dump();
                    } else if (strcmp(args, "dump") == 0) {
                        msg_cache_debug_dump();
                    } else if (strcmp(args, "clear") == 0) {
                        if (msg_store_clear()) {
                            LOGI("msg_store SPIFFS file cleared");
                        }
                    } else if (strncmp(args, "spiffs", 6) == 0) {
                        // 'cache spiffs' / 'cache spiffs on' / 'cache spiffs off'
                        char* sub = args + 6;
                        while (*sub == ' ') sub++;  // skip whitespace
                        if (*sub == 0) {
                            // 상태만 출력: user / runtime / effective 3단계
                            LOGI("spiffs_cache: user=%s, runtime=%s, effective=%s",
                                 storage_get_spiffs_cache_enabled() ? "ON" : "OFF",
                                 storage_get_spiffs_runtime_disabled() ? "DISABLED(battery CRIT)" : "OK",
                                 storage_get_spiffs_cache_effective() ? "ON" : "OFF");
                            LOGI("  (user: NVS-persisted, runtime: RAM-only auto by battery, effective=actual)");
                        } else if (strcmp(sub, "on") == 0 || strcmp(sub, "enable") == 0) {
                            if (storage_set_spiffs_cache_enabled(true)) {
                                LOGI("spiffs_cache user=ON (persisted, effective=%s)",
                                     storage_get_spiffs_cache_effective() ? "ON" : "OFF (runtime disabled)");
                            } else {
                                LOGE("failed to set spiffs_cache=ON (NVS error)");
                            }
                        } else if (strcmp(sub, "off") == 0 || strcmp(sub, "disable") == 0) {
                            if (storage_set_spiffs_cache_enabled(false)) {
                                LOGI("spiffs_cache user=OFF (persisted, effective=OFF)");
                                LOGI("기존 SPIFFS 데이터는 유지되어 flush 시 전달됨");
                            } else {
                                LOGE("failed to set spiffs_cache=OFF (NVS error)");
                            }
                        } else {
                            LOGE("usage: cache spiffs [on|off]");
                        }
                    } else {
                        LOGE("usage: cache [status|dump|clear|spiffs [on|off]]");
                    }
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
    Serial.println(" v18 (pointer-based queue + v17 TX power/dump)");
    Serial.println(" build: " __DATE__ " " __TIME__);
#if defined(BOARD_VARIANT_ESP32C3)
    Serial.println(" target: ESP32-C3 (RISC-V, BLE 5.0)");
#elif defined(BOARD_VARIANT_ESP32S3)
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

    // SPIFFS 마운트 - 블랙리스트 영속화 + 향후 파일 저장 용도
    //   - partitions/no_ota_spiffs_1mb.csv: spiffs 1024KB @ 0x2F0000
    //   - true = format if mount fails (첫 플래싱 시 자동 포맷)
    if (!SPIFFS.begin(true)) {
        LOGE("SPIFFS mount failed, blacklist/node_config/wifi will be degraded");
    } else {
        // 사용량 표시 (arduino-esp32 v2.x: totalBytes()/usedBytes())
        LOGI("SPIFFS mounted: used=%u / total=%u bytes",
             (unsigned)SPIFFS.usedBytes(),
             (unsigned)SPIFFS.totalBytes());
    }

    // 노드 프로필 + WiFi/MQTT 설정 로드 (/node.conf, /wifi.conf)
    // - SPIFFS 파일 기반, 변경 시에만 write
    // - NVS(bwifi)에 기존 WiFi 자격증명이 있으면 1회 자동 마이그레이션
    node_config_init();
    {
        const char* nick = node_config_get_nickname();
        const bc_wifi_config_t* wifi = node_config_get_wifi();
        LOGI("node profile: nickname='%s'", nick[0] ? nick : "(not set)");
        LOGI("wifi config: ssid='%s', broker=%s:%u, prefix=%s",
             wifi->ssid[0] ? wifi->ssid : "(not set)",
             wifi->mqtt_broker, wifi->mqtt_port,
             wifi->mqtt_topic_prefix);
    }

    // BLE relay 시작 (내부에서 blacklist_init() → SPIFFS에서 로드)
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

    // 30초마다 상태 요약 (v17: 5초 → 30초로 로그 최소화)
    if (millis() - g_last_status_ms > 30000) {
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
