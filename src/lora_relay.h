/*
 * lora_relay.h - LoRa 모듈 자동 감지 + BLE↔LoRa 브릿지
 *
 * 한국어:
 *   arduino-LoRa (Sandeep Mistry) 라이브러리 기반.
 *   여러 알려진 LoRa 모듈 핀아웃(Heltec, TTGO, T-Beam 등)을 순차 시도하여
 *   SX1276/77/78/RFM95W 칩을 자동 인식.
 *
 *   동작:
 *     1) begin() - 다양한 핀아웃으로 SPI 초기화 + LoRa.begin() 시도
 *        - SX127x의 RegVersion(0x42) 읽어 0x12이면 정상 인식
 *        - 첫 번째로 성공한 설정 사용
 *     2) onReceive 콜백 등록 - LoRa로 들어온 패킷을 BLE 큐에 enqueue
 *     3) broadcast_packet() - BLE relay에서 패킷 중계 시 LoRa로도 송신
 *
 *   주파수:
 *     - 기본 868MHz (EU KR 920-923MHz 대역)
 *     - 환경 변수 또는 begin(frequency)로 변경 가능
 *     - US: 915MHz, KR: 921MHz
 *
 *   한계:
 *     - SX127x만 지원 (SX1262/1268은 다른 라이브러리 필요)
 *     - 단일 주파수 채널 (FHSS 미구현)
 *     - 큰 패킷(>222B)은 packet_splitter로 분할/재조립
 *     - v23: inter-chunk delay 200ms + TX 재시도 + 타임아웃 15s
 */

#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include "bitchat_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// LoRa 모듈 핀 정의 (자동 감지용 후보)
typedef struct {
    const char* name;
    int8_t  sck, miso, mosi, cs, rst, irq;
    bool     uses_spi_begin;   // false = LoRa.setPins()만, true = SPI.begin() 호출
} lora_pinout_t;

// 주파수 사전 설정
#define LORA_FREQ_EU_868   868E6
#define LORA_FREQ_US_915   915E6
#define LORA_FREQ_KR_921   921E6

// LoRa 설정값 (기본값)
#define LORA_TX_POWER_DBM   17
#define LORA_BANDWIDTH_HZ   125000
#define LORA_SPREADING_FACTOR 7
#define LORA_CODING_RATE    5
#define LORA_SYNC_WORD      0x34
#define LORA_PREAMBLE_LEN   8

// LoRa 프레임 최대 크기 (SX127x: 256 bytes FIFO)
#define LORA_MAX_FRAME_LEN  222

// 초기화 - 여러 핀아웃 시도하여 첫 번째로 인식되는 모듈 사용
// frequency_hz: 0 = 기본 868MHz
// 성공 시 true, 모든 핀아웃 실패 시 false
bool lora_relay_begin(uint32_t frequency_hz = LORA_FREQ_EU_868);

// LoRa 모듈 인식 여부
bool lora_relay_is_detected(void);

// 인식된 모듈 이름 (Heltec v2, TTGO T-Beam 등)
const char* lora_relay_get_module_name(void);

// BLE relay가 패킷을 forward할 때마다 LoRa로도 브로드캐스트
// data: bitchat 패킷 (binary), len: 길이
// LORA_MAX_FRAME_LEN 초과 시 drop
bool lora_relay_broadcast(const uint8_t* data, size_t len);

// LoRa 수신 핸들러 (라이브러리 콜백에서 호출됨)
// 이 함수는 bitchat_pkt_event_t 큐에 패킷을 넣음 (BLE relay와 동일한 큐)
void lora_relay_handle_receive(int packet_size);

// 디버그 정보 출력
void lora_relay_debug_dump(void);

#ifdef __cplusplus
}
#endif
