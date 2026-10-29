/*
 * wifi_bridge.h - WiFi + MQTT 브릿지 (BLE ↔ 인터넷)
 *
 * 한국어:
 *   WiFi STA 모드로 AP에 연결 → MQTT 브로커에 연결
 *   BLE 메시 패킷을 MQTT topic으로 publish, subscribe한 패킷을 BLE 큐로 전달
 *
 *   Serial 명령으로 on/off 토글 가능 (wifi on / wifi off / wifi stat)
 *
 *   MQTT 토픽:
 *     bchat/rx/{node_id}  - 이 노드로 들어오는 패킷 (subscribe)
 *     bchat/tx            - 브로드캐스트 (publish, 다른 노드가 subscribe)
 *
 *   BLE ↔ WiFi 동시 사용 (coex):
 *     sdkconfig에서 BLE 우선 Optimum 모드 설정
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// WiFi 설정 (컴파일 타임 또는 NVS에서)
// 컴파일 타임: -DBC_WIFI_SSID="..." -DBC_WIFI_PASS="..."
// NVS: "bwifi" namespace, "ssid"/"pass" keys (향후 확장)
#define BC_WIFI_NVS_NS  "bwifi"
#define BC_WIFI_NVS_SSID "ssid"
#define BC_WIFI_NVS_PASS "pass"

// MQTT 설정 (기본값, 컴파일 타임 override 가능)
#ifndef BC_MQTT_BROKER
#define BC_MQTT_BROKER  "test.mosquitto.org"
#endif
#ifndef BC_MQTT_PORT
#define BC_MQTT_PORT    1883
#endif
#ifndef BC_MQTT_TOPIC_PREFIX
#define BC_MQTT_TOPIC_PREFIX  "bchat"
#endif

// 초기화 - WiFi + MQTT 모듈 준비 (실제 연결은 begin_connection 호출 시)
bool wifi_bridge_init(void);

// WiFi + MQTT 연결 시도 (Serial 명령 'wifi on'에서 호출)
// ssid/pass가 NULL이면 컴파일 타임 매크로 또는 NVS에서 조회
bool wifi_bridge_begin_connection(const char* ssid, const char* password);

// 연결 해제 (Serial 명령 'wifi off'에서 호출)
void wifi_bridge_disconnect(void);

// 연결 상태
bool wifi_bridge_is_connected(void);
bool wifi_bridge_is_enabled(void);   // 사용자가 켰는지 (init 후 begin_connection 안 했으면 false)

// 메인 루프에서 호출 - MQTT keep-alive + 수신 패킷 처리
void wifi_bridge_loop(void);

// BLE relay가 패킷을 forward할 때마다 MQTT로도 publish
// data: bitchat 패킷 (binary), len: 길이
// base64 인코딩해서 MQTT payload로 전송 (binary 안전)
bool wifi_bridge_publish(const uint8_t* data, size_t len);

// 디버그 정보 출력
void wifi_bridge_debug_dump(void);

// WiFi 설정을 NVS에 저장 (Serial 명령 'wifi set <ssid> <pass>'에서 사용)
// 구버전 호환용 - 실제로는 node_config_set_wifi_credentials() 호출 (SPIFFS)
bool wifi_bridge_save_credentials(const char* ssid, const char* password);

// NVS에서 WiFi 설정 조회
// 구버전 호환용 - 실제로는 node_config_get_wifi()에서 SPIFFS 조회 (fallback으로 NVS 확인)
bool wifi_bridge_load_credentials(char* out_ssid, size_t ssid_max,
                                   char* out_pass, size_t pass_max);

// ----- MQTT 설정 (SPIFFS 영속화) -----
// 'wifi broker <host> [port]' 명령에서 사용
// 변경 즉시 SPIFFS(/wifi.conf)에 저장, 다음 연결 시 적용
bool wifi_bridge_set_broker(const char* broker, uint16_t port);

// 'wifi prefix <prefix>' 명령 - MQTT topic prefix 변경 (영속화)
bool wifi_bridge_set_topic_prefix(const char* prefix);

// 'wifi tls on/off' 명령 - mTLS 활성화 토글
// on=true: WiFiClientSecure + 인증서 로드 (port 8883)
// on=false: WiFiClient plain (port 1883, 기본값)
// 인증서 파일 필요: /broker_ca.pem (필수), /client_cert.pem + /client_key.pem (mTLS 시)
bool wifi_bridge_set_tls_enabled(bool on);
bool wifi_bridge_get_tls_enabled(void);

// 현재 MQTT broker 설정 반환 (캐시)
const char* wifi_bridge_get_broker(void);
uint16_t    wifi_bridge_get_port(void);
const char* wifi_bridge_get_topic_prefix(void);

// 인증서 파일 존재 여부 확인 (node_config_check_certs 위임)
// 반환: bitmask (1=CA, 2=client_cert, 4=client_key)
int wifi_bridge_check_certs(void);

#ifdef __cplusplus
}
#endif
