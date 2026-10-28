/*
 * power.h - 전원 관리 모듈 (배터리 + 절전 모드)
 *
 * 한국어:
 *   - ESP-PM(POWER MANAGEMENT)를 사용하여 자동 라이트 슬립
 *   - BLE 컨트롤러는 계속 켜두고 CPU만 idle 시 슬립 → 패킷 수신 시 즉시 wake
 *   - 배터리 전압 ADC 모니터링 (선택)
 *   - 상태 LED 저듀티 펄스 (1초에 50ms만 on = 5% 듀티)
 *   - 5분 이상 무활동 시 딥슬립 진입 (GPIO 0 버튼으로 wake)
 *
 * 전력 소비 예상:
 *   - BLE advertising 1.28s 간격 + 라이트 슬립: ~5-10mA (ESP32)
 *   - 18650 3000mAh 배터리로 약 12-25일 동작 가능
 *
 * 컴파일 옵션:
 *   -DBC_POWER_BATTERY=1  : 배터리 모드 활성화 (기본값)
 *   -DBC_POWER_USB_ALWAYS=1 : USB 상시 전원 모드 (이전 동작 유지)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 상태 LED 핀 (ESP32 DevKitC의 온보드 LED)
// C3는 보통 GPIO 8 또는 2, ESP32 클래식은 GPIO 2
#ifdef BC_BOARD_IS_C3
#define BC_LED_PIN          8       // ESP32-C3-DevKitM-1 온보드 LED
#else
#define BC_LED_PIN          2       // ESP32 DevKitC V4 온보드 LED
#endif

// 배터리 ADC 설정 (GPIO 35 = ADC1_CH7, 전압 분배 2:1)
//   LiPo 4.2V → ADC 2.1V → 11dB attenuation으로 측정 가능
#define BC_BATTERY_ADC_PIN   35
#define BC_BATTERY_DIVIDER_R1 100000.0f  // 100kΩ (배터리 측)
#define BC_BATTERY_DIVIDER_R2 100000.0f  // 100kΩ (GND 측)
// 분배비 = (R1+R2)/R2 = 2.0 → ADC 값 * 2 = 실제 배터리 전압

// 배터리 임계값 (LiPo)
#define BC_BATTERY_V_FULL    4.20f
#define BC_BATTERY_V_MID      3.75f
#define BC_BATTERY_V_LOW      3.40f   // 경고
#define BC_BATTERY_V_CRIT      3.10f   // BLE 활동 중지

// 딥슬립 진입 임계 유휴 시간 (ms)
#define BC_DEEP_SLEEP_IDLE_MS  (5 * 60 * 1000)   // 5분

typedef struct {
    bool     battery_mode;        // true: 배터리+절전, false: USB 상시
    uint32_t adv_interval_ms;     // BLE advertising 간격 (1000ms = 저전력)
    uint32_t scan_window_ms;      // 스캔 윈도우 (한 주기당 스캔 시간)
    uint32_t scan_interval_ms;    // 스캔 주기
    uint8_t  status_led_pin;      // 0xFF = 비활성
    uint8_t  battery_adc_pin;     // 0xFF = 비활성
    uint32_t deep_sleep_idle_ms;  // 0 = 딥슬립 비활성, >0 = N ms 후 진입
} power_config_t;

// 기본 배터리 모드 설정 반환 (main.cpp에서 사용)
void power_get_default_config(power_config_t* cfg);

// 초기화 - ESP-PM 설정 + ADC 설정 + LED 설정
bool power_init(const power_config_t* cfg);

// 메인 루프에서 호출 - 유휴 시 라이트 슬립 진입 판정
// 마지막 활동 시간 갱신은 power_mark_active()로
void power_handle_idle(void);

// 패킷 수신/송신 등 활동이 있을 때마다 호출
// 마지막 활동 타임스탬프 갱신 (딥슬립 진입 판정에 사용)
void power_mark_active(void);

// LED 짧은 펄스 - 패킷 처리 시 (50ms on, 메인 루프에서 자동 꺼짐)
void power_led_pulse_packet(void);

// LED 상태 - alive 표시 (1초당 50ms on)
void power_led_set_alive(bool on);

// 배터리 전압 읽기 (V), 측정 불가 시 -1.0f
float power_read_battery_voltage(void);

// 배터리 상태 간단 리포트 (Serial 출력)
void power_battery_debug(void);

// 딥슬립 진입 - us 마이크로초 후 자동 wake + GPIO 0 버튼 wake
// 주의: 딥슬립 중에는 BLE 끊김, RAM 초기화됨 (RTC 메모리만 유지)
void power_enter_deep_sleep(uint64_t sleep_us);

#ifdef __cplusplus
}
#endif
