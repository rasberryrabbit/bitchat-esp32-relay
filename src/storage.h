/*
 * storage.h - NVS persistence for keys / peer list / config
 *
 * 한국어:
 *   - 자체 Ed25519 키쌍을 최초 부팅 시 한 번 생성하고 NVS에 저장
 *   - 이후 부팅 때마다 NVS에서 복원
 *   - 피어 공개키 캐시 (선택 사항 - 화이트리스트 용도)
 *   - 카운터 / 통계 저장 (선택 사항)
 *
 * 자체 키 영속화 크기: 64B privkey + 32B pubkey = 96B
 *   (Ed25519는 결정론적 키가 아니므로 두 값을 모두 저장해야 함)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BC_NVS_NAMESPACE  "bchat"
#define BC_NVS_KEY_PRIV    "priv"     // 64-byte Ed25519 private key
#define BC_NVS_KEY_PUB    "pub"      // 32-byte Ed25519 public key
#define BC_NVS_KEY_COUNTER "ctr"
#define BC_NVS_KEY_PEERS  "peers"
#define BC_NVS_KEY_CONFIG "cfg"
#define BC_NVS_KEY_BAT_IGN "bat_ign"  // 배터리 상태 무시 플래그 (uint8_t, 0/1)
#define BC_NVS_KEY_SFF_EN  "sff_en"   // store-and-forward SPIFFS 캐시 enable (uint8_t, 0/1, 기본 1=ON)
#define BC_NVS_KEY_DS_EN   "dpslp"    // 딥슬립 허용 플래그 (uint8_t, 0/1, 기본 0=OFF)
#define BC_NVS_KEY_TXPWR   "txpwr"    // BLE TX power (int8_t dBm, 기본 9=+9dBm)
#define BC_NVS_KEY_NETMODE "nmode"    // net transport mode (uint8_t, 0=off 1=socket 2=mqtt, 기본 1)
#define BC_NVS_KEY_SOCKHOST "shost"   // socket server host (string, 최대 64B)
#define BC_NVS_KEY_SOCKPORT "sport"   // socket server port (uint16_t, 기본 8765)
#define BC_MAX_PEERS       32  // 최대 캐시 피어 수

typedef struct {
    uint8_t  sender_id[8];     // bitchat peerID (8 bytes)
    uint8_t  reserved[24];   // struct size preserved
} bitchat_peer_t;

typedef struct {
    bool           ready;
    bitchat_keypair_t self_kp;       // 자체 키 (RAM 캐시)
    uint64_t       self_counter;     // 패킷 counter
    bitchat_peer_t peers[BC_MAX_PEERS];
    uint8_t        peer_count;
} bitchat_storage_t;

// 최초 1회 호출: NVS open + 키 복원/생성
bool storage_init(bitchat_storage_t* st);

// 자체 키 쌍 반환 (영속화된 키)
const bitchat_keypair_t* storage_self_keypair(const bitchat_storage_t* st);

// 카운터 1 증가 + NVS 동기화. 새 패킷 송신 시마다 호출
uint64_t storage_next_counter(bitchat_storage_t* st);

// 새 피어 공개키 추가 (중복이면 false 반환)
bool storage_add_peer(bitchat_storage_t* st, const uint8_t* pk);

// 피어 목록에서 공개키 검색
bool storage_has_peer(const bitchat_storage_t* st, const uint8_t* pk);

// 디버그용 - Serial에 저장 상태 출력
void storage_debug_dump(const bitchat_storage_t* st);

// ----- 배터리 상태 무시 플래그 (NVS 영속화) -----
//
//   배터리 전압이 임계치(3.10V) 이하로 떨어져도 BLE 활동을 중지하지 않도록
//   강제하는 플래그. USB 상시 전원 환경이나 배터리 전압 분배가 잘못된 보드,
//   혹은 디버깅 목적으로 BLE를 계속 띄우고 싶을 때 사용.
//
//   상태: NVS에 uint8_t (0=off, 1=on)로 저장. 재부팅 후에도 유지.
//   기본값: 0 (off, 정상 동작)
//
//   적용 지점:
//     - power.cpp: V_CRIT 도달 시 BLE 활동 중지 액션 스킵
//     - power.cpp: V_LOW 도달 시 경고 로그는 출력하되 조치는 취하지 않음
//     - main.cpp: 'bat ignore on/off/status' Serial 명령으로 토글

// 배터리 무시 플래그 읽기 (true=무시, false=정상)
bool storage_get_battery_ignore(void);

// 배터리 무시 플래그 설정 (NVS에 저장)
// 성공 시 true 반환
bool storage_set_battery_ignore(bool ignore);

// ----- store-and-forward SPIFFS 캐시 enable 플래그 (NVS 영속화) -----
//
//   msg_cache의 RAM overflow를 SPIFFS에 영속화할지 말지를 제어.
//   기본값: ON (SPIFFS 캐시 활성화)
//
//   OFF일 때 동작:
//     - RAM 가득 차도 SPIFFS에 write 안 함 → 그냥 drop (broadcast와 동일)
//     - 이미 SPIFFS에 있는 레코드는 부팅 시/클라이언트 접속 시 그대로 전달 (유지)
//     - 즉, OFF는 "새로운 SPIFFS write 금지"만 의미, 기존 데이터는 유지/전달
//
//   ON일 때 동작 (기본):
//     - RAM 가득 차면 가장 오래된 direct message를 SPIFFS로 overflow
//     - 부팅 시 SPIFFS → RAM 복원
//     - 클라이언트 접속 시 RAM + SPIFFS 모두 flush
//
//   사용 사례:
//     - 플래시 수명 극단적 보호: `cache spiffs off` → SPIFFS write 0건
//     - 배터리 부족 + SPIFFS 마모 우려 시 일시적 비활성화
//     - 디버깅: store-and-forward 기능 자체를 끄고 싶을 때

bool storage_get_spiffs_cache_enabled(void);  // 사용자 설정 (NVS 영속, 기본 true)
bool storage_set_spiffs_cache_enabled(bool enabled);

// ----- SPIFFS 캐시 런타임 오버라이드 (RAM만, NVS에 저장 안 함) -----
//
//   배터리 CRIT 등 시스템 상황에 따라 임시로 SPIFFS 쓰기를 금지.
//   사용자의 NVS 설정(user_setting)은 보존되고, 런타임 오버라이드만 적용/해제.
//
//   실제 동작(effective) = user_setting AND NOT runtime_disabled
//     - user=ON, runtime=false → effective=ON (정상)
//     - user=ON, runtime=true  → effective=OFF (배터리 CRIT로 임시 비활성)
//     - user=OFF, runtime=any  → effective=OFF (사용자가 명시적으로 끔)
//
//   배터리 회복 시 runtime_disabled=false → 자동으로 user_setting 복귀
//   (사용자가 수동으로 cache spiffs on 할 필요 없음)

// 런타임 오버라이드 설정 (RAM만, 재부팅 시 false로 리셋)
void storage_set_spiffs_runtime_disabled(bool disabled);

// 런타임 오버라이드 상태 조회
bool storage_get_spiffs_runtime_disabled(void);

// 실제 동작 상태 = user_setting AND NOT runtime_disabled
// msg_cache.cpp 등에서 SPIFFS 쓰기 여부 판단 시 이 함수 사용
bool storage_get_spiffs_cache_effective(void);

// ----- 딥슬립 허용 플래그 (NVS 영속화) -----
//
//   5분간 무활동 시 자동 딥슬립 진입을 허용할지 말지를 제어.
//   기본값: OFF (BLE 메시 릴레이 노드에 부적합 - 딥슬립 시 BLE 끊김, RAM 손실)
//
//   ESP32 딥슬립 특성 (주의):
//     - wake 시 무조건 reset 발생 (RAM 초기화, setup()부터 재실행)
//     - BLE 컨트롤러 끊김 → 모든 peer 연결 해제
//     - RAM 상태 (peer_keys, msg_cache, retry_queue) 전부 손실
//     - ~1초 재부팅 시간 동안 메시지 유실 가능
//
//   OFF일 때 동작 (기본):
//     - 5분 idle 임계치 도달해도 딥슬립 진입 안 함
//     - 대신 esp_pm의 auto light sleep이 유휴 시 CPU만 절전 (BLE 컨트롤러는 계속 동작)
//     - RAM 상태 보존, BLE 연결 유지, 메시지 유실 없음
//
//   ON일 때 동작:
//     - 5분 idle 시 60초 딥슬립 → wake 후 reset → setup()부터 재실행
//     - 극단적 배터리 절약이 필요한 경우 (예: 장기 미사용 시)
//     - 대신 모든 상태 손실 감수
//
//   사용 사례:
//     - 릴레이 노드: OFF 유지 (기본값) - BLE continuity 우선
//     - 가끔 깨어나는 센서 노드: ON - 배터리 우선
//     - 배터리 부족 시 긴급 절전: ON 후 재부팅 시 USB 연결하여 충전

bool storage_get_deep_sleep_enabled(void);  // 기본 false (OFF)
bool storage_set_deep_sleep_enabled(bool enabled);

// ----- BLE TX power 설정 (NVS 영속화) -----
//
//   NimBLE 출력 전력을 dBm 단위로 설정. ESP32 classic은 -12~+9 dBm,
//   ESP32-C3/S3는 -12~+12 dBm 지원. 허용 범위 밖 값은 클램프.
//   기본값: +9 dBm (발견 확률 최우선, 연결 후엔 advertising 안 나가므로
//   실제 송출 시간 = 미연결 유휴 시간뿐이라 전력 소모 한계적).
//
//   적용 시점:
//     - ble_relay.cpp begin(): NVS에서 로드하여 NimBLEDevice::setPower() 호출
//     - main.cpp 'txpower <N>' Serial 명령으로 런타임 변경 + NVS 저장
//     - 변경 즉시 NimBLEDevice::setPower() 재적용 (advertising 재시작 불필요)
//
//   사용 사례:
//     - 멀리 있는 폰 발견: txpower 9 (최대, 기본값)
//     - 배터리 절약 + 근거리: txpower 0 또는 3
//     - 극단적 절전: txpower -3

int8_t storage_get_tx_power(void);              // 기본 +9 dBm
bool storage_set_tx_power(int8_t dbm);          // NVS에 저장 + ble_relay에 즉시 적용

// ----- v19: net transport mode (NVS 영속화) -----
//   0=OFF, 1=SOCKET (기본, 직접 TCP), 2=MQTT (브로커)
uint8_t storage_get_net_mode(void);             // 기본 1 (SOCKET)
bool storage_set_net_mode(uint8_t mode);

// ----- v19: socket server host/port (NVS 영속화) -----
//   socket 모드에서 연결할 서버 주소. host 최대 63바이트 + NUL.
//   host가 비어 있으면 socket 모드 비활성화.
bool storage_get_socket_host(char* out, size_t out_max);  // 기본 "" (빈 문자열)
bool storage_set_socket_host(const char* host);
uint16_t storage_get_socket_port(void);                    // 기본 8765
bool storage_set_socket_port(uint16_t port);

#ifdef __cplusplus
}
#endif
