/*
 * ble_relay.h - BLE GATT server + scanner + mesh relay engine (NimBLE 기반)
 *
 * 한국어:
 *   NimBLE 스텍 사용 (Bluedroid 대신) - 메모리 1/3 절약
 *   - C3에서 4 conn → 8-12 conn 가능 (heap 고갈 한계까지)
 *   - 동일한 동작: GATT server advertise + 클라이언트 write 수신
 *   - 동일한 흐름: scan → connect → notify 구독 → 패킷 중계
 *
 * NimBLE vs Bluedroid 주요 차이점:
 *   - BLE2902 (CCCD) 명시적 생성 불필요 - NimBLE이 PROPERTY_NOTIFY 기반 자동 처리
 *   - BLEAdvertisedDeviceCallbacks::onResult 가 포인터 (값 아님)
 *   - BLEAddress::getNative()가 const uint8_t* 직접 반환 (esp_bd_addr_t* 아님)
 *   - scan interval/window가 ms 단위 (Bluedroid는 0.625ms 단위)
 *   - advertising interval은 여전히 0.625ms 단위 (setMinPreferred/setMaxPreferred)
 */

#pragma once

#include <Arduino.h>
#include "NimBLEDevice.h"
#include "NimBLEServer.h"
#include "NimBLEClient.h"
#include "NimBLEUtils.h"
#include "NimBLEScan.h"
#include "NimBLEAdvertisedDevice.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "bitchat_protocol.h"
#include "crypto.h"
#include "storage.h"

// 자체 BLE Service / Characteristic UUID (Bluedroid과 동일)
#define BC_BLE_SERVICE_UUID   "0000bc00-0000-1000-8000-00805f9b34fb"
#define BC_BLE_TX_CHAR_UUID   "0000bc01-0000-1000-8000-00805f9b34fb"  // notify (server -> clients)
#define BC_BLE_RX_CHAR_UUID   "0000bc02-0000-1000-8000-00805f9b34fb"  // write (clients -> server)
#define BC_BLE_ADV_NAME       "bchat-relay"

// NimBLE 사용 시 동시 연결 상향
//   - Bluedroid: SRAM 제약으로 C3=4, classic/S3=7
//   - NimBLE: CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9 + NimBLE이 client/server 통합 관리
//     따라서 9개를 in/out으로 분배. ESP32 BLE 컨트롤러 최대 9개.
#ifdef BC_BOARD_IS_C3
#define BC_MAX_OUT_CONN       4   // C3 + NimBLE: 4 outgoing (안전 마진) (Bluedroid 2 → 4)
#define BC_MAX_IN_CONN        4   // C3 + NimBLE: 4 incoming (안전 마진) (Bluedroid 2 → 5)
                                  // 총 9 (NimBLE 한계 9 중 사용)
#elif defined(BC_BOARD_IS_S3)
#define BC_MAX_OUT_CONN       4   // S3 + NimBLE: 4 outgoing (안전 마진)
#define BC_MAX_IN_CONN        4   // S3 + NimBLE: 4 incoming (안전 마진)
                                  // 총 9 (NimBLE 한계 9 중 사용)
#else
#define BC_MAX_OUT_CONN       4   // ESP32 클래식 (안전 마진) + NimBLE: 4 outgoing
#define BC_MAX_IN_CONN        4   // ESP32 클래식 (안전 마진) + NimBLE: 5 incoming
                                  // 총 9 (NimBLE 한계 9 중 사용)
#endif

// 중복 패킷 dedup 캐시 크기 (LRU)
#define BC_DEDUP_CACHE_SIZE   64

// 패킷 수신 큐 길이 (FreeRTOS queue)
#ifdef BC_BOARD_IS_C3
#define BC_PKT_QUEUE_LEN      12   // C3 + NimBLE: 8 → 12 (heap 여유)
#else
#define BC_PKT_QUEUE_LEN      16
#endif

// 패킷 출처 (어느 링크로 들어왔는지)
enum BitchatPktSource : uint8_t {
    BC_SRC_SERVER_RX = 0,   // 클라이언트가 우리 서버에 write 한 패킷
    BC_SRC_CLIENT_NOTIFY,  // 우리가 연결한 peer가 notify로 보낸 패킷
    BC_SRC_LOCAL,           // 자체 생성
};

typedef struct {
    uint8_t  data[BITCHAT_MAX_PKT_LEN];
    size_t   len;
    uint8_t  source;
    uint16_t src_conn_id;
} bitchat_pkt_event_t;

class BitchatRelay {
public:
    BitchatRelay();
    ~BitchatRelay();

    bool begin(bitchat_storage_t* storage);
    void loop();
    int incoming_count() const { return _incoming_count; }
    int outgoing_count() const { return _outgoing_count; }

    // Public for external modules (LoRa relay) - 패킷을 BLE 처리 큐에 enqueue
    void enqueue_packet(const uint8_t* data, size_t len,
                        BitchatPktSource src, uint16_t conn_id);

private:
    bitchat_storage_t* _storage;
    NimBLEServer*         _server;
    NimBLEService*        _service;
    NimBLECharacteristic* _tx_char;     // notify
    NimBLECharacteristic* _rx_char;     // write
    NimBLEAdvertising*    _advertising;
    NimBLEScan*           _scan;
    QueueHandle_t         _pkt_queue;

    volatile int      _incoming_count;
    volatile int      _outgoing_count;
    bool               _scan_active;

    struct PeerEntry {
        uint8_t  addr[6];
        bool     addr_valid;
        NimBLEClient*  client;
        NimBLERemoteCharacteristic* tx_remote;
        NimBLERemoteCharacteristic* rx_remote;
        bool        connected;
        uint32_t    last_seen_ms;
        uint32_t    pkts_forwarded;

        PeerEntry() : addr_valid(false), client(nullptr),
                      tx_remote(nullptr), rx_remote(nullptr),
                      connected(false), last_seen_ms(0),
                      pkts_forwarded(0) {
            memset(addr, 0, sizeof(addr));
        }
    };
    PeerEntry _peers[BC_MAX_OUT_CONN];
    uint8_t   _peer_slot_count;

    struct DedupEntry {
        uint8_t  key[16];
        uint32_t ts;
        bool     used;
    };
    DedupEntry _dedup[BC_DEDUP_CACHE_SIZE];
    uint32_t   _dedup_idx;

    void process_packet(bitchat_pkt_event_t* evt);
    bool verify_packet(const uint8_t* data, size_t len,
                       bitchat_packet_view_t* view);
    bool check_dedup(const uint8_t* sender_pk, uint64_t counter);
    void forward_to_others(const uint8_t* data, size_t len, uint16_t except_conn);
    void forward_to_peer(PeerEntry* p, const uint8_t* data, size_t len);
    void scan_cycle();
    void maintain_connections();
    bool connect_to_peer(NimBLEAddress addr);
    void disconnect_peer(PeerEntry* p);

    friend class BitchatServerCallbacks;
    friend class BitchatRXCallbacks;
    friend class BitchatAdvertisedCallbacks;
    friend class BitchatClientCallbacks;
};

extern BitchatRelay g_relay;
