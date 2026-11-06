/*
 * power.cpp - 전원 관리 구현 (배터리 + 절전)
 *
 * 한국어:
 *   - esp_pm_locks를 통한 자동 라이트 슬립
 *   - main loop에서 tick마다 power_handle_idle() 호출 → CPU가 idle일 때
 *     BLE 컨트롤러는 계속 동작하지만 CPU/플래시/DRAM 클럭이 자동으로 내려감
 *   - 배터리 전압은 1분마다 한 번씩 측정 (자주 읽으면 전력 소모)
 *   - 5분간 아무 활동 없으면 딥슬립 진입 → GPIO 0(BOOT 버튼) 또는
 *     60초 타이머로 wake
 */

#include "power.h"
#include "log.h"
#include "storage.h"   // storage_get_battery_ignore(), storage_get_temp_threshold()
#include "ble_relay.h" // v34: ble_relay_set_thermal_mode()
#include "wifi_bridge.h" // v34: wifi_bridge_is_enabled(), wifi_bridge_disconnect()

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp32-hal-adc.h>
#include <driver/rtc_io.h>
#include <string.h>

// ESP-IDF target 자동 감지 (CONFIG_IDF_TARGET_ESP32C3 등)
// arduino-esp32 빌드 시 자동으로 정의됨
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(BOARD_VARIANT_ESP32C3)
#define BC_POWER_IS_C3 1
#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(BOARD_VARIANT_ESP32S3)
#define BC_POWER_IS_S3 1
#endif

static power_config_t g_cfg;
static bool g_init_done = false;
static esp_pm_lock_handle_t g_pm_light_lock = nullptr;
static volatile uint32_t g_last_active_ms = 0;
static uint32_t g_last_battery_check_ms = 0;
static uint32_t g_last_led_pulse_ms = 0;
static uint32_t g_last_alive_pulse_ms = 0;
static bool g_led_state = false;
static float g_last_battery_v = 0.0f;
// 배터리 무시 플래그 캐시 (boot 시 NVS에서 1회 로드, 이후 RAM 조회)
static bool g_battery_ignore_cache = false;
static bool g_battery_ignore_loaded = false;
// 딥슬립 스킵 안내 로그 출력 여부 (첫 임계치 도달 시 1회만)
static bool g_logged_deep_sleep_skipped = false;

void power_get_default_config(power_config_t* cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
#ifdef BC_POWER_USB_ALWAYS
    cfg->battery_mode = false;
    cfg->adv_interval_ms = 100;     // 빠른 발견
    cfg->scan_window_ms  = 40;
    cfg->scan_interval_ms = 80;
    cfg->status_led_pin  = 0xFF;     // LED 비활성
    cfg->battery_adc_pin = 0xFF;
    cfg->deep_sleep_idle_ms = 0;     // 딥슬립 없음
#else
    // 기본 배터리 모드
    cfg->battery_mode = true;
    cfg->adv_interval_ms = 1280;    // 1.28s (BLE 0x800 units)
    cfg->scan_window_ms  = 30;      // 한 주기당 30ms만 스캔
    cfg->scan_interval_ms = 1280;   // 1.28s 주기
    cfg->status_led_pin  = BC_LED_PIN;
    cfg->battery_adc_pin = BC_BATTERY_ADC_PIN;
    cfg->deep_sleep_idle_ms = BC_DEEP_SLEEP_IDLE_MS;
#endif
}

bool power_init(const power_config_t* cfg) {
    if (g_init_done) return true;
    if (!cfg) return false;
    memcpy(&g_cfg, cfg, sizeof(g_cfg));

    // ----- v29: DFS (Dynamic Frequency Scaling) 항상 적용 -----
    // 배터리/USB 모드 관계없이 DFS를 켜서 CPU 유휴 시 클럭 자동 강하 → 발열 감소
    // light_sleep는 BLE와 호환 안 됨(0x0106) → DFS만 켜고 light_sleep는 끔
    if (!g_cfg.battery_mode) {
        LOGI("power: USB always-on mode (DFS enabled, light sleep disabled)");
    } else {
        LOGI("power: battery mode (DFS enabled, light sleep disabled)");
    }

#ifdef BC_POWER_IS_C3
    // v31: C3의 유효 DFS 주파수는 80/160MHz만. 40MHz는 무효 → 0x0106 에러 원인.
    // max=80, min=80 → CPU 항상 80MHz 고정 (DFS 효과 없지만 에러 없음)
    // 발열은 f_cpu=80MHz + BLE 주기 느리게로 충분히 감소
    esp_pm_config_esp32c3_t pm_cfg;
    memset(&pm_cfg, 0, sizeof(pm_cfg));
    pm_cfg.max_freq_mhz       = 80;      // C3 최대 80MHz
    pm_cfg.min_freq_mhz       = 80;      // v31: 40→80 (40MHz는 C3 무효 주파수)
    pm_cfg.light_sleep_enable = false;   // light sleep 비활성 (BLE 비호환)
    esp_err_t rc = esp_pm_configure(&pm_cfg);
#elif defined(BC_POWER_IS_S3)
    esp_pm_config_esp32s3_t pm_cfg;
    memset(&pm_cfg, 0, sizeof(pm_cfg));
    pm_cfg.max_freq_mhz       = 160;     // v29: S3 최대 160MHz (발열 감소, 240→160)
    pm_cfg.min_freq_mhz       = 80;      // S3 최저 80MHz (DFS 유휴 시)
    pm_cfg.light_sleep_enable = false;   // light sleep 비활성 (BLE 비호환)
    esp_err_t rc = esp_pm_configure(&pm_cfg);
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    rc = ESP_OK;
#else
    esp_pm_config_esp32_t pm_cfg;
    memset(&pm_cfg, 0, sizeof(pm_cfg));
    pm_cfg.max_freq_mhz       = 240;     // ESP32 클래식 최대
    pm_cfg.min_freq_mhz       = 80;
    pm_cfg.light_sleep_enable = false;   // v29: light sleep 비활성 (BLE 호환성)
    esp_err_t rc = esp_pm_configure(&pm_cfg);
#endif
    if (rc != ESP_OK) {
        LOGW("esp_pm_configure failed: 0x%04x (continuing without DFS)", rc);
    } else {
        LOGI("power: DFS configured (max=%d→min=%d MHz, light sleep off)",
#ifdef BC_POWER_IS_C3
             80, 40
#elif defined(BC_POWER_IS_S3)
             160, 80
#else
             240, 80
#endif
        );
    }

    // USB 상시 모드는 여기서 리턴 (배터리 전용 기능 스킵)
    if (!g_cfg.battery_mode) {
        g_init_done = true;
        return true;
    }

    // ----- 상태 LED 핀 -----
    if (g_cfg.status_led_pin != 0xFF) {
        pinMode(g_cfg.status_led_pin, OUTPUT);
        digitalWrite(g_cfg.status_led_pin, LOW);
        LOGI("power: status LED on GPIO %u", g_cfg.status_led_pin);
    }

    // ----- 배터리 ADC -----
    if (g_cfg.battery_adc_pin != 0xFF) {
        analogReadResolution(12);            // 12-bit (0..4095)
        analogSetAttenuation(ADC_11db);      // 0..3.3V 입력 (정확히는 ~3.6V)
        LOGI("power: battery ADC on GPIO %u", g_cfg.battery_adc_pin);
        g_last_battery_v = power_read_battery_voltage();
        power_battery_debug();
    }

    // ----- 딥슬립 웨이크업 소스 등록 -----
    if (g_cfg.deep_sleep_idle_ms > 0) {
        // 모든 ESP32 변형에서 호환되는 timer-only wakeup 사용
        // (ext0는 클래식 전용, ext1은 C3에서 deprecated 모드 필요 등 보드별 차이)
        // 60초 후 자동 wake - power_enter_deep_sleep 호출 시 설정
        // GPIO 0 버튼은 wake-up 역할 대신 hard reset으로 동작
        LOGI("power: deep sleep armed after %u ms idle, wake = 60s timer (hard reset via BOOT btn)",
             (unsigned)g_cfg.deep_sleep_idle_ms);
    }

    g_last_active_ms = millis();
    g_init_done = true;
    LOGI("power: battery+saving mode initialized");
    return true;
}

// v34: 온도 기반 스마트 전력 제어
//   10초마다 칩 내부 온도를 읽어 4단계 열 구간 판정 → BLE/WiFi 전력 동적 조정
static uint32_t g_last_temp_check_ms = 0;
static float g_last_temp_c = 0.0f;
static bool g_thermal_monitor_enabled = true;  // 기본 ON

// 온도 구간 판정 → thermal_mode 반환 (0-3)
static uint8_t temp_to_thermal_mode(float temp_c, uint8_t threshold) {
    // 히스테리시스: 임계 - 5°C에서 복귀 (급격한 토글 방지)
    if (temp_c >= threshold + 10) return 3;       // CRITICAL: 임계+10°C
    if (temp_c >= threshold + 5)  return 2;       // HOT: 임계+5°C
    if (temp_c >= threshold)      return 1;       // WARM: 임계 도달
    if (temp_c >= threshold - 5)  return 0;       // COOL 복귀 (히스테리시스)
    return 0;
}

void power_handle_idle(void) {
    if (!g_init_done) return;

    uint32_t now = millis();

    // v34: 온도 모니터링 (10초마다, 모든 모드에서 실행)
    if (g_thermal_monitor_enabled && (now - g_last_temp_check_ms > 10000)) {
        g_last_temp_check_ms = now;
        g_last_temp_c = temperatureRead();

        uint8_t threshold = storage_get_temp_threshold();
        uint8_t new_mode = temp_to_thermal_mode(g_last_temp_c, threshold);
        uint8_t cur_mode = ble_relay_get_thermal_mode();

        // 모드 변경 시에만 로그 + BLE 파라미터 조정
        if (new_mode != cur_mode) {
            LOGI("thermal: %.1f°C → mode %u (threshold=%u°C, hysteresis=%u°C)",
                 g_last_temp_c, new_mode, threshold, threshold - 5);
            ble_relay_set_thermal_mode(new_mode);

            // CRITICAL 모드에서 WiFi 강제 OFF
            if (new_mode >= 2 && wifi_bridge_is_enabled()) {
                LOGW("thermal: HOT/CRITICAL → WiFi forced OFF");
                wifi_bridge_disconnect();
                WiFi.mode(WIFI_OFF);
                esp_wifi_stop();
            }
        }
    }

    if (!g_cfg.battery_mode) return;  // USB 모드는 여기까지 (딥슬립 로직 스킵)

    uint32_t idle_ms = now - g_last_active_ms;

    // 5분 이상 무활동 시 딥슬립 진입 (deep_sleep_enabled 플래그가 ON일 때만)
    // 기본 OFF: BLE 메시 릴레이 노드는 RAM/BLE 연결을 잃지 않기 위해
    // esp_pm의 auto light sleep만 사용 (CPU만 절전, BLE 컨트롤러는 동작 유지)
    if (g_cfg.deep_sleep_idle_ms > 0 &&
        idle_ms > g_cfg.deep_sleep_idle_ms &&
        storage_get_deep_sleep_enabled()) {
        LOGW("idle %u ms > threshold %u, deep_sleep=ON → entering deep sleep",
             idle_ms, g_cfg.deep_sleep_idle_ms);
        power_enter_deep_sleep(60ULL * 1000000ULL);   // 60초 후 자동 wake
        // power_enter_deep_sleep이 리턴하지 않음 - 시스템 재부팅
    } else if (g_cfg.deep_sleep_idle_ms > 0 &&
               idle_ms > g_cfg.deep_sleep_idle_ms &&
               !storage_get_deep_sleep_enabled()) {
        // deep_sleep 플래그 OFF - 딥슬립 진입 안 함
        // auto light sleep (esp_pm)이 CPU만 절전, BLE는 계속 동작
        // 첫 임계치 도달 시 한 번만 로그 출력
        if (!g_logged_deep_sleep_skipped) {
            LOGI("idle %u ms > threshold %u but deep_sleep=OFF, staying in light sleep",
                 idle_ms, g_cfg.deep_sleep_idle_ms);
            LOGI("(enable via 'sleep deep on' if extreme battery saving needed)");
            g_logged_deep_sleep_skipped = true;
        }
    }

    // 상태 LED: alive 펄스 (1초당 50ms on)
    if (g_cfg.status_led_pin != 0xFF) {
        if (!g_led_state && (now - g_last_alive_pulse_ms > 1000)) {
            g_last_alive_pulse_ms = now;
            digitalWrite(g_cfg.status_led_pin, HIGH);
            g_led_state = true;
        } else if (g_led_state && (now - g_last_alive_pulse_ms > 50)) {
            digitalWrite(g_cfg.status_led_pin, LOW);
            g_led_state = false;
        }
    }

    // 패킷 펄스 (50ms on 후 자동 off)
    if (g_cfg.status_led_pin != 0xFF &&
        g_led_state && g_last_led_pulse_ms > 0 &&
        (now - g_last_led_pulse_ms > 50)) {
        digitalWrite(g_cfg.status_led_pin, LOW);
        g_led_state = false;
        g_last_led_pulse_ms = 0;
    }

    // 배터리 전압 1분마다 측정
    if (g_cfg.battery_adc_pin != 0xFF &&
        (now - g_last_battery_check_ms > 60000)) {
        g_last_battery_check_ms = now;
        g_last_battery_v = power_read_battery_voltage();

        // 배터리 무시 플래그 확인 (NVS에서 로드, power_init 시 캐시)
        // true이면 임계치 도달해도 BLE 중지/딥슬립 액션 스킵
        bool bat_ignored = power_is_battery_ignored();

        if (g_last_battery_v > 0 && g_last_battery_v < BC_BATTERY_V_LOW) {
            if (bat_ignored) {
                LOGW("battery low: %.2fV (IGNORED by flag)", g_last_battery_v);
            } else {
                LOGW("battery low: %.2fV", g_last_battery_v);
            }
        }
        if (g_last_battery_v > 0 && g_last_battery_v < BC_BATTERY_V_CRIT) {
            if (bat_ignored) {
                LOGE("battery critical: %.2fV - BLE keep-alive forced (battery_ignore=ON)",
                     g_last_battery_v);
                // battery_ignore 플래그가 있으면 BLE 중지 액션 스킵
                // 정상 동작 시 아래 TODO 라인이 활성화되어야 함
            } else {
                LOGE("battery critical: %.2fV, stopping BLE activity", g_last_battery_v);
                // TODO: BLE 끄고 딥슬립 진입 - 향후 확장
            }

            // 배터리 CRIT 시 공통 액션 (battery_ignore 여부와 무관):
            // SPIFFS 캐시 런타임 비활성화 - 플래시 마모 보호
            // NVS에 저장하지 않고 RAM에만 설정 → 회복 시 자동 복귀
            // (사용자의 cache spiffs on/off 설정 보존)
            if (!storage_get_spiffs_runtime_disabled()) {
                storage_set_spiffs_runtime_disabled(true);
                LOGW("battery CRIT: spiffs_cache runtime disabled (user setting preserved, auto-recover on battery restore)");
            }
        } else if (g_last_battery_v > 0 && g_last_battery_v >= BC_BATTERY_V_CRIT + 0.1f) {
            // 전압이 CRIT 위로 회복 시 런타임 오버라이드 해제
            // → effective = user_setting (사용자가 켜놨었으면 자동으로 다시 ON)
            if (storage_get_spiffs_runtime_disabled()) {
                storage_set_spiffs_runtime_disabled(false);
                LOGI("battery recovered above CRIT (%.2fV) - spiffs_cache runtime override cleared, effective=%s",
                     g_last_battery_v,
                     storage_get_spiffs_cache_effective() ? "ON" : "OFF");
            }
        }
    }

    // 라이트 슬립 진입 시도 - 1ms delay로 FreeRTOS tickless idle 트리거
    // (esp_pm_configure가 자동 처리하므로 delay만 주면 됨)
    delay(1);
}

bool power_is_battery_ignored(void) {
    if (!g_battery_ignore_loaded) {
        // lazy load - 첫 조회 시 storage_get_battery_ignore() 호출하여 NVS에서 로드
        // storage_init()이 선행되어야 함
        g_battery_ignore_cache = storage_get_battery_ignore();
        g_battery_ignore_loaded = true;
    }
    return g_battery_ignore_cache;
}

void power_reload_battery_ignore(void) {
    // 사용자가 'bat ignore on/off' 명령으로 변경 시 호출
    // 캐시 무효화 → 다음 power_is_battery_ignored() 호출 시 NVS에서 다시 로드
    g_battery_ignore_loaded = false;
}

void power_mark_active(void) {
    if (!g_init_done) return;
    g_last_active_ms = millis();
}

void power_led_pulse_packet(void) {
    if (!g_init_done || g_cfg.status_led_pin == 0xFF) return;
    uint32_t now = millis();
    g_last_led_pulse_ms = now;
    digitalWrite(g_cfg.status_led_pin, HIGH);
    g_led_state = true;
}

void power_led_set_alive(bool on) {
    if (!g_init_done || g_cfg.status_led_pin == 0xFF) return;
    digitalWrite(g_cfg.status_led_pin, on ? HIGH : LOW);
    g_led_state = on;
}

float power_read_battery_voltage(void) {
    if (!g_init_done || g_cfg.battery_adc_pin == 0xFF) return -1.0f;
    // 3회 평균
    uint32_t sum = 0;
    const int N = 3;
    for (int i = 0; i < N; ++i) {
        sum += analogRead(g_cfg.battery_adc_pin);
        delay(2);
    }
    float adc_v = (float)(sum / N) * 3.3f / 4095.0f;
    // 분배비 보정: V_battery = V_adc * (R1+R2)/R2
    float ratio = (BC_BATTERY_DIVIDER_R1 + BC_BATTERY_DIVIDER_R2)
                  / BC_BATTERY_DIVIDER_R2;
    return adc_v * ratio;
}

void power_battery_debug(void) {
    if (g_last_battery_v <= 0) {
        LOGI("battery: N/A");
        return;
    }
    const char* level;
    if (g_last_battery_v >= BC_BATTERY_V_FULL - 0.05f)      level = "FULL";
    else if (g_last_battery_v >= BC_BATTERY_V_MID)         level = "HIGH";
    else if (g_last_battery_v >= BC_BATTERY_V_LOW)         level = "MID";
    else if (g_last_battery_v >= BC_BATTERY_V_CRIT)       level = "LOW";
    else                                                   level = "CRIT";
    LOGI("battery: %.2fV [%s]", g_last_battery_v, level);
}

void power_enter_deep_sleep(uint64_t sleep_us) {
    if (sleep_us > 0) {
        esp_sleep_enable_timer_wakeup(sleep_us);
    }
    LOGI("entering deep sleep for %llu us...", (unsigned long long)sleep_us);
    Serial.flush();
    esp_deep_sleep_start();
    // 리턴하지 않음 - 웨이크업 시 setup()부터 재실행
}
