/*
 * msg_store.h - SPIFFS-backed overflow cache for store-and-forward
 *
 * 한국어:
 *   msg_cache(RAM 32개)가 가득 찼을 때 추가 메시지를 SPIFFS에 영속화.
 *   재부팅 후 RAM 캐시가 비어 있을 때 SPIFFS에서 로드 → 자동 복원.
 *
 *   플래시 수명 보호 설계 (제한적 동작):
 *     1. RAM 우선 - 정상 동작 시 SPIFFS 쓰기 발생 안 함
 *     2. 단일 파일 /msg_store.bin - 여러 파일 분산 쓰기보다 erase cycle 최소화
 *     3. 고정 크기 레코드 - in-place 갱신, fragment 안정
 *     4. 최대 64개 한계 - 용량 16.5KB 제한
 *     5. 패킷 크기 한계 256B - 큰 패킷(브로드캐스트)은 drop
 *     6. direct message 우선 - broadcast는 SPIFFS에 안 쓰고 drop
 *
 *   플래시 마모 계산 (worst case):
 *     - 4KB 섹터당 100,000 erase cycle
 *     - SPIFFS 16.5KB → ~5개 섹터 사용 → 500K erase cycle
 *     - worst: RAM overflow 1회/분 = 500K 분 = 347 일
 *     - typical: 10 msgs/day overflow = 137 년
 *
 *   동작 흐름:
 *     boot → msg_store_load_to_ram() → 모든 SPIFFS 레코드를 RAM 캐시로 옮기고
 *              SPIFFS 파일 truncate (부팅 후 SPIFFS는 비어 있음)
 *     msg_cache_add() 호출 시 RAM이 가득 찼으면:
 *       → 가장 오래된 RAM entry를 msg_store_add()로 밀어냄
 *       → 새 메시지를 RAM에 추가
 *     client connect → msg_cache_flush():
 *       → RAM 먼저 flush
 *       → msg_store_flush_to_callback()로 SPIFFS의 모든 레코드도 flush
 *       → 양쪽 모두 초기화
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 단일 파일 - 모든 레코드가 /msg_store.bin에 들어감
#define MSG_STORE_FILE           "/msg_store.bin"

// 최대 레코드 수 - 3072 (기존 64 → 48배 확장)
// 3072 × 264B = 792KB (SPIFFS 1MB 중 79% 사용, 다른 파일 10KB와 합쳐 ~802KB)
#define MSG_STORE_MAX_ENTRIES    3072

// SPIFFS에 저장할 패킷 최대 크기 - 이보다 큰 패킷은 drop (RAM에만 캐시)
// 일반 bitchat 메시지 페이로드는 100-200B, 256B면 충분
#define MSG_STORE_MAX_PKT_LEN    256

// 레코드 크기 = 1B active + 4B ts + 2B len + 256B data = 263B
// 4바이트 정렬을 위해 264B로 패딩
#define MSG_STORE_RECORD_SIZE    264

// 초기화 - SPIFFS 마운트 확인 (msg_cache_init 선행)
// SPIFFS.begin()은 main.cpp setup()에서 호출됨
bool msg_store_init(void);

// 현재 활성 레코드 수
size_t msg_store_count(void);

// 레코드 추가 - 가장 오래된 슬롯에 덮어쓰기 (in-place update)
// 패킷이 MSG_STORE_MAX_PKT_LEN보다 크면 false 반환 (drop)
// SPIFFS가 가득 차면(3072/3072) 가장 오래된 레코드 덮어쓰기
bool msg_store_add(const uint8_t* data, size_t len);

// 부팅 시 호출 - SPIFFS의 모든 레코드를 RAM 캐시로 로드 시도
// emit_fn이 true 반환 → 해당 레코드를 SPIFFS에서 inactive 마킹 (RAM에 적재됨)
// emit_fn이 false 반환 → 해당 레코드를 SPIFFS에 그대로 유지 (RAM 가득 찬 경우)
//   이 경우 나중에 클라이언트 접속 시 msg_cache_flush()가 SPIFFS 레코드도 함께 flush
// 반환: RAM으로 옮긴 레코드 수 (나머지는 SPIFFS에 남음)
typedef bool (*msg_store_emit_fn)(const uint8_t* data, size_t len);
size_t msg_store_load_to_ram(msg_store_emit_fn emit_fn);

// flush - 모든 SPIFFS 레코드를 callback으로 방출 후 파일 truncate
// 클라이언트가 메시지를 소비할 때 사용 (msg_cache_flush에서 호출)
// emit_fn은 void 반환 (방출 후 무조건 제거 - 클라이언트가 받았다고 간주)
// 반환: 방출된 레코드 수
size_t msg_store_flush_to_callback(void (*emit_fn)(const uint8_t* data, size_t len));

// 파일 삭제 (전체 초기화)
bool msg_store_clear(void);

// 디버그 출력
void msg_store_debug_dump(void);

#ifdef __cplusplus
}
#endif
