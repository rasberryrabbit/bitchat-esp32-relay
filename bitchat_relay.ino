/*
 * bitchat_relay.ino - Arduino IDE entry sketch
 *
 * 한국어:
 *   Arduino IDE에서 빌드하기 위한 진입 sketch입니다.
 *   PlatformIO의 src/main.cpp와 동일한 내용이며,
 *   Arduino IDE는 .ino 파일의 setup()/loop()를 자동으로 호출합니다.
 *
 * 사용법 (Arduino IDE):
 *   1. 이 파일이 있는 폴더를 통째로 Arduino IDE에서 엽니다.
 *      - 폴더명과 동일한 .ino 파일이 필요합니다.
 *   2. 같은 폴더에 모든 .cpp/.h 파일이 있어야 Arduino IDE가
 *      자동으로 같이 컴파일합니다. (src/ 하위가 아닌 sketch 폴더 루트)
 *   3. 보드 선택: "ESP32 Dev Module" 또는 "ESP32C3 Dev Module"
 *   4. Tools > Custom Build Properties에 build.flags=-DBC_POWER_BATTERY=1
 *      (배터리 모드 활성화, 기본값)
 *   5. Upload
 *
 * 자세한 사항은 README.md의 "Arduino IDE 빌드" 섹션을 참고하세요.
 */

#include <Arduino.h>
#include "log.h"
#include "crypto.h"
#include "storage.h"
#include "ble_relay.h"
#include "bitchat_protocol.h"
#include "power.h"

static bitchat_storage_t g_storage;
static power_config_t   g_power;
static uint32_t g_last_status_ms = 0;

void setup() {
    // 시리얼 초기화 - USB serial (디버깅/최초 설정용)
    // 배터리 모드에서도 USB가 연결된 동안은 로그 출력
    Serial.begin(115200);
    delay(100);
    Serial.println("\r\n=============================");
    Serial.println(" bitchat ESP32 relay firmware");
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

    crypto_init();

    power_get_default_config(&g_power);
    if (!power_init(&g_power)) {
        LOGW("power_init failed, continuing without power management");
    }

    if (!storage_init(&g_storage)) {
        LOGE("storage_init failed, halting");
        while (true) { delay(1000); }
    }
    storage_debug_dump(&g_storage);

    if (!g_relay.begin(&g_storage)) {
        LOGE("BLE relay init failed, halting");
        while (true) { delay(1000); }
    }
    LOGI("=== bitchat relay node ready ===");
}

void loop() {
    g_relay.loop();
    power_handle_idle();

    if (millis() - g_last_status_ms > 5000) {
        g_last_status_ms = millis();
        LOGI("--- status: in=%d out=%d ctr=%llu peers=%u ---",
             g_relay.incoming_count(),
             g_relay.outgoing_count(),
             (unsigned long long)g_storage.self_counter,
             g_storage.peer_count);
        if (g_power.battery_adc_pin != 0xFF) {
            power_battery_debug();
        }
    }
}
