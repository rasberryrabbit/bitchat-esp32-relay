/*
 * retry_queue.h - ACK 재전송 로직 (패시브 ACK + 타임아웃 기반)
 *
 * 한국어:
 *   메시 프로토콜에서 사용하는 두 가지 ACK 메커니즘을 결합:
 *
 *   1) 패시브 ACK (Passive Acknowledgment):
 *      우리가 forward한 패킷이 다른 peer에 의해 다시 브로드캐스트되는 것을
 *      감지하면 "성공적으로 전달되었다"로 간주하고 재전송 큐에서 제거.
 *      (BLE mesh, Thread, Zigbee 등에서 사용하는 패턴)
 *
 *   2) 명시적 ACK (Explicit ACK, type=0x02):
 *      bitchat ACK 패킷이 도착하면 payload에서 원본 (sender_pk, counter)를
 *      추출해 우리 pending entry와 매칭. 최종 목적지 도달을 의미.
 *
 *   3) 타임아웃 재전송:
 *      pending entry가 BC_RETRY_TIMEOUT_MS(2초) 내에 ACK를 받지 못하면
 *      BC_RETRY_MAX_RETRIES(3회)까지 재전송. 그 후 drop.
 *
 *   중요: dedup_cache(30초 LRU)와 별개로 동작.
 *         dedup은 "이미 본 패킷인가?" 추적
 *         retry_queue는 "내가 forward한 패킷이 ACK 왔는가?" 추적
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "bitchat_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// 재전송 큐 설정
#define BC_RETRY_QUEUE_SIZE    16    // pending entry 최대 수
#define BC_RETRY_TIMEOUT_MS    2000  // 2초 - 단일 홉 예상 시간
#define BC_RETRY_MAX_RETRIES   3     // 최대 3회 재시도 = 총 4회 송신
#define BC_RETRY_HASH_LEN      16    // sender_pk 앞 8B + counter 8B = 16B

// pending entry - 우리가 forward한 패킷 추적
typedef struct {
    uint8_t  hash[BC_RETRY_HASH_LEN];     // 패킷 식별용 해시
    uint8_t  packet[BITCHAT_MAX_PKT_LEN]; // 재전송용 전체 패킷 (TTL 감소된 버전)
    size_t   packet_len;
    uint32_t first_sent_ms;               // 최초 송신 시각
    uint32_t last_sent_ms;                // 마지막 송신 시각
    uint8_t  retry_count;                  // 현재까지 재시도 횟수
    bool     active;
} retry_entry_t;

// 재전송 콜백 - pending entry가 타임아웃되면 호출
// user_data: BitchatRelay 포인터 (forward_to_others 호출을 위해)
typedef void (*retry_resend_fn)(const uint8_t* packet, size_t len,
                                 void* user_data);

// 통계
typedef struct {
    uint32_t pending_count;       // 현재 대기 중인 entry 수
    uint32_t total_enqueued;      // 총 enqueue 횟수
    uint32_t total_passive_acks;  // 패시브 ACK로 제거된 수
    uint32_t total_explicit_acks; // 명시적 ACK로 제거된 수
    uint32_t total_retransmits;   // 타임아웃 재전송 횟수
    uint32_t total_dropped;       // 최대 재시도 초과로 drop된 수
} retry_stats_t;

// 초기화 - 최초 1회 호출
void retry_init(void);

// 패킷 forward 후 호출 - pending 큐에 추가
// packet: TTL 감소된 (forward용) 패킷
// 호출자: ble_relay.cpp의 process_packet() 마지막에
void retry_enqueue(const uint8_t* packet, size_t len);

// 패시브 ACK 체크
// 수신한 패킷이 우리가 forward했던 것과 같은 (sender_pk, counter)라면
// pending entry를 제거하고 true 반환.
// 호출자: process_packet()의 dedup 체크 전에 호출
bool retry_check_passive_ack(const uint8_t* sender_pk, uint64_t counter);

// 명시적 ACK 패킷 처리
// ACK 패킷의 payload에서 원본 (sender_pk, counter)를 추출해 매칭
// payload 형식: sender_pk(32B) || counter(8B) || ack_data(옵션)
// 매칭 성공 시 pending entry 제거하고 true 반환
bool retry_process_explicit_ack(const uint8_t* ack_payload, size_t len);

// 주기적 타임아웃 체크 - main loop에서 호출
// timed-out entry를 찾아 resend 콜백 호출, retry_count++
// 최대 재시도 초과 시 entry 제거
void retry_check_timeouts(retry_resend_fn resend, void* user_data);

// 디버그 - Serial에 큐 상태 출력
void retry_debug_dump(void);

// 통계 조회
void retry_get_stats(retry_stats_t* out_stats);

// 큐 비우기 - 테스트/리셋용
void retry_clear(void);

#ifdef __cplusplus
}
#endif
