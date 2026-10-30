/*
 * node_config.h - SPIFFS-based configuration for node profile + WiFi/MQTT settings
 *
 * 한국어:
 *   노드 닉네임과 WiFi/MQTT 설정을 SPIFFS 파일로 영속화.
 *   기존 NVS 기반 wifi_bridge 저장을 대체 + 새로 노드 닉네임 기능 추가.
 *
 *   파일 2개 (key=value INI-like 포맷, 사람이 읽기 쉬움):
 *     /node.conf - 노드 프로필 (nickname 등)
 *     /wifi.conf - WiFi + MQTT 설정 (ssid, pass, broker, port, prefix)
 *
 *   파일 포맷 예시 (/wifi.conf):
 *     # bitchat relay wifi/mqtt config
 *     ssid=MyHomeWiFi
 *     pass=password123
 *     broker=test.mosquitto.org
 *     port=1883
 *     topic_prefix=bchat
 *
 *   플래시 수명:
 *     - 설정 변경 시에만 write (사용자가 'wifi set' 또는 'node nick' 입력 시)
 *     - boot 시 read-only로 load → RAM 캐시
 *     - 영속 데이터: nickname (~32B) + wifi config (~200B) → 총 ~250B
 *     - 일반적 사용: 하루 1-2회 변경 → 사실상 마모 무시 가능
 *
 *   backward compat:
 *     - 기존 NVS(bwifi namespace)에 저장된 ssid/pass가 있으면 부팅 시 1회 자동 마이그레이션
 *       → SPIFFS로 옮기고 NVS에서 삭제
 *     - 이후 모든 설정은 SPIFFS-only
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 파일 경로 (SPIFFS 루트)
#define NODE_CONFIG_FILE    "/node.conf"
#define WIFI_CONFIG_FILE    "/wifi.conf"

// 길이 제한
#define MAX_NICKNAME_LEN    32
#define MAX_WIFI_SSID_LEN   64
#define MAX_WIFI_PASS_LEN   64
#define MAX_MQTT_BROKER_LEN 64
#define MAX_TOPIC_PREFIX_LEN 16
#define MAX_SOCKET_HOST_LEN 64

// 노드 프로필
typedef struct {
    char nickname[MAX_NICKNAME_LEN + 1];   // 빈 문자열이면 미설정
} node_profile_t;

// v19: net transport mode (SPIFFS /wifi.conf에 저장)
typedef enum {
    BC_NET_MODE_OFF    = 0,
    BC_NET_MODE_SOCKET = 1,   // 기본값 - 직접 TCP 소켓
    BC_NET_MODE_MQTT   = 2,   // MQTT 브로커
} bc_net_mode_t;

// v20: socket protocol (SOCKET 모드 내에서 TCP vs UDP vs UDP-bcast)
typedef enum {
    BC_NET_PROTO_TCP       = 0,  // 기본값 - length-prefix TCP (신뢰성 내장)
    BC_NET_PROTO_UDP       = 1,  // 유니캐스트 UDP + ACK 재전송 (host 필요)
    BC_NET_PROTO_UDP_BCAST = 2,  // 브로드캐스트 UDP + ACK (LAN 자체 완결, host 불필요)
} bc_net_proto_t;

// WiFi + MQTT + socket 설정 (SPIFFS /wifi.conf에 영속화)
typedef struct {
    char     ssid[MAX_WIFI_SSID_LEN + 1];
    char     pass[MAX_WIFI_PASS_LEN + 1];
    char     mqtt_broker[MAX_MQTT_BROKER_LEN + 1];
    uint16_t mqtt_port;
    char     mqtt_topic_prefix[MAX_TOPIC_PREFIX_LEN + 1];
    bool     use_tls;              // true: mTLS (port 8883), false: plain (port 1883)
    // v19: socket 직접 통신 설정
    uint8_t  net_mode;             // 0=off, 1=socket(기본), 2=mqtt
    char     socket_host[MAX_SOCKET_HOST_LEN + 1];  // socket 서버 주소 (빈=미설정)
    uint16_t socket_port;          // socket 서버 포트 (기본 8765)
    // v20: socket protocol (SOCKET 모드 내에서)
    uint8_t  net_proto;            // 0=tcp(기본), 1=udp, 2=udp-bcast
} bc_wifi_config_t;

// 초기화 - SPIFFS 마운트 + 파일 로드 + NVS 마이그레이션 (1회)
// msg_store_init()과 병행 호출 가능 (SPIFFS.begin()은 idempotent)
bool node_config_init(void);

// ----- 노드 프로필 -----

// /node.conf에서 프로필 로드 (없으면 빈 프로필 반환)
bool node_config_load_profile(node_profile_t* profile);

// /node.conf에 프로필 저장
bool node_config_save_profile(const node_profile_t* profile);

// 캐시된 nickname 반환 (빈 문자열이면 미설정)
// boot 시 1회 로드 후 RAM 캐시, 이후 빠른 조회
const char* node_config_get_nickname(void);

// nickname 설정 (RAM 캐시 + SPIFFS 영속화)
// 빈 문자열이면 nickname 제거
bool node_config_set_nickname(const char* nickname);

// ----- WiFi/MQTT 설정 -----

// /wifi.conf에서 설정 로드 (없으면 기본값)
// 기본값: 빈 ssid/pass, broker=test.mosquitto.org, port=1883, prefix=bchat
bool node_config_load_wifi(bc_wifi_config_t* cfg);

// /wifi.conf에 설정 저장
bool node_config_save_wifi(const bc_wifi_config_t* cfg);

// 캐시된 WiFi config 반환 (boot 시 로드)
// 반환된 포인터는 영구적 (전역 정적 변수)
const bc_wifi_config_t* node_config_get_wifi(void);

// 개별 필드만 업데이트 + 자동 save
// 편의 함수 - Serial 명령에서 사용
bool node_config_set_wifi_credentials(const char* ssid, const char* pass);
bool node_config_set_mqtt_broker(const char* broker, uint16_t port);
bool node_config_set_mqtt_topic_prefix(const char* prefix);

// ----- TLS (mTLS MQTT) 설정 -----
//
//   use_tls=true 시 MQTT 연결에 mTLS 사용:
//     - port: 자동으로 8883 (또는 사용자 지정)
//     - 인증서: SPIFFS의 PEM 파일에서 로드
//       /broker_ca.pem  - 브로커 CA 인증서 (브로커 검증)
//       /client_cert.pem - 클라이언트 인증서 (브로커가 클라이언트 검증)
//       /client_key.pem  - 클라이언트 개인키
//     - WiFiClientSecure 사용, PubSubClient에 주입
//
//   use_tls=false (기본): plain MQTT, port 1883, WiFiClient 사용
//
//   인증서 업로드:
//     Serial 명령 `cert upload <ca|client|key>` → PEM 내용 붙여넣기
//     또는 SPIFFS 이미지 빌드 시 포함

// TLS 활성화 토글 (wifi.conf의 use_tls 필드, NVS에 영속화)
// on=true → use_tls=1, port=8883 (기본) / on=false → use_tls=0, port=1883
bool node_config_set_tls_enabled(bool on);

// 현재 TLS 설정 조회 (캐시)
bool node_config_get_tls_enabled(void);

// ----- v19: net transport mode + socket 설정 (SPIFFS /wifi.conf에 영속화) -----
//   net_mode: 0=off, 1=socket(기본), 2=mqtt
//   socket_host: socket 서버 주소 (빈 문자열=미설정)
//   socket_port: socket 서버 포트 (기본 8765)
bool node_config_set_net_mode(uint8_t mode);       // wifi.conf에 저장
uint8_t node_config_get_net_mode(void);            // 캐시에서 조회 (기본 1=SOCKET)
bool node_config_set_socket_host(const char* host); // wifi.conf에 저장
const char* node_config_get_socket_host(void);     // 캐시에서 조회
bool node_config_set_socket_port(uint16_t port);   // wifi.conf에 저장
uint16_t node_config_get_socket_port(void);        // 캐시에서 조회 (기본 8765)

// ----- v20: socket protocol (SPIFFS /wifi.conf에 영속화) -----
//   net_proto: 0=tcp(기본), 1=udp, 2=udp-bcast
bool node_config_set_net_proto(uint8_t proto);     // wifi.conf에 저장
uint8_t node_config_get_net_proto(void);           // 캐시에서 조회 (기본 0=TCP)

// 인증서 파일 존재 여부 확인 (SPIFFS)
// 반환: 0=none, 1=CA만, 3=CA+client_cert, 7=CA+cert+key (전체)
// mTLS에 필요한 최소: 7 (CA + client cert + client key)
// CA-only 모드도 가능 (서버만 검증, 클라이언트 인증 생략) - 1 이상이면 TLS 가능
int node_config_check_certs(void);

#ifdef __cplusplus
}
#endif
