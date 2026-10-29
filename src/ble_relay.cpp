/*
 * ble_relay.cpp - BLE mesh relay implementation (NimBLE 기반)
 *
 * 한국어:
 *   NimBLE-Arduino 라이브러리를 사용한 BLE GATT server/client + mesh relay.
 *   Bluedroid와의 주요 차이점:
 *   - CCCD descriptor (BLE2902) 자동 처리 - PROPERTY_NOTIFY 설정만으로 충분
 *   - onResult callback이 pointer 인자
 *   - scan interval/window가 ms 단위 (Bluedroid는 0.625ms 단위)
 *   - setValue/notify API는 거의 동일
 */

#include "ble_relay.h"
#include "log.h"
#include "power.h"
#include "retry_queue.h"
#include "blacklist.h"
#include "lora_relay.h"
#include "wifi_bridge.h"
#include "peer_keys.h"   // senderID → Ed25519 pk 매핑
#include "msg_cache.h"    // store-and-forward 메시지 캐시

#include <cstring>
#include <algorithm>
#include <string>

BitchatRelay g_relay;

// ============================================================================
// Server callbacks (incoming connections)
// ============================================================================
class BitchatServerCallbacks : public NimBLEServerCallbacks {
public:
    BitchatServerCallbacks(BitchatRelay* r) : _r(r) {}
    // NimBLE 1.4.2: onConnect/onDisconnect 단일 인자 (ble_gap_conn_desc* 옵션)
    void onConnect(NimBLEServer* server) override {
        _r->_incoming_count++;
        _r->_last_connect_ms = millis();   // store-and-forward flush 2초 지연용
        LOGI("client connected, in=%d out=%d",
             _r->_incoming_count, _r->_outgoing_count);
    }
    void onDisconnect(NimBLEServer* server) override {
        if (_r->_incoming_count > 0) _r->_incoming_count--;
        LOGI("client disconnected, in=%d", _r->_incoming_count);
        // advertise 재시작 (NimBLE은 자동으로 안 함)
        server->getAdvertising()->start();
    }
private:
    BitchatRelay* _r;
};

// ============================================================================
// RX characteristic callbacks (clients write to us)
// ============================================================================
// BLE write accumulator: 큰 패킷(이미지 등)이 여러 BLE write로 분할되어
// 전송되는 것을 누적하여 완전한 패킷으로 조립 후 처리
// 2개 패킷 분량 확보 (연속으로 오는 패킷 처리용)
#define BLE_ACC_BUF_SIZE  (2 * BITCHAT_MAX_PKT_LEN + 128)  // 2*808 + 128 = 1744B

class BitchatRXCallbacks : public NimBLECharacteristicCallbacks {
public:
    BitchatRXCallbacks(BitchatRelay* r) : _r(r), _acc_len(0) {}

    void onWrite(NimBLECharacteristic* ch) override {
        NimBLEAttValue val = ch->getValue();
        if (val.size() == 0) return;
        const uint8_t* data = val.data();
        size_t len = val.size();
        LOGD("RX write: %u bytes", (unsigned)len);

        if (len > BLE_ACC_BUF_SIZE) {
            _r->enqueue_packet(data, len, BC_SRC_SERVER_RX, 0xFFFF);
            return;
        }

        // 0) 새 데이터 추가 전에 기존 버퍼에서 완전한 패킷 추출 (공간 확보)
        //    이 단계가 없으면 2개 패킷이 버퍼에 쌓였을 때 overflow 발생
        extract_packets();

        // 1) 새 데이터 추가
        if (_acc_len + len > BLE_ACC_BUF_SIZE) {
            // 추출 후에도 공간 부족 → 버퍼 리셋 (비정상 상황)
            LOGW("BLE acc: overflow (%u+%u > %u) after extraction, resetting",
                 (unsigned)_acc_len, (unsigned)len, BLE_ACC_BUF_SIZE);
            _acc_len = 0;
        }
        memcpy(_acc_buf + _acc_len, data, len);
        _acc_len += len;

        // 2) 새 데이터 추가 후 다시 추출 시도
        extract_packets();
    }

private:
    BitchatRelay* _r;
    uint8_t _acc_buf[BLE_ACC_BUF_SIZE];
    size_t _acc_len;

    // 누적 버퍼에서 완전한 패킷 추출 (0개 이상 추출 가능)
    void extract_packets() {
        int skip_count = 0;  // 연속 스킵 카운터 (가비지 동기화용)
        while (_acc_len >= BITCHAT_MIN_PKT_LEN) {
            uint8_t ver = _acc_buf[0];
            if (ver != BITCHAT_VERSION_1 && ver != BITCHAT_VERSION_2) {
                _acc_len--;
                memmove(_acc_buf, _acc_buf + 1, _acc_len);
                skip_count++;
                continue;
            }

            bitchat_packet_view_t v;
            if (!bitchat_parse(_acc_buf, _acc_len, &v)) {
                // 파싱 실패: 데이터가 더 필요하거나 가비지
                // 버퍼가 max 패킷 크기보다 크면 가비지로 간주하고 1바이트 스킵
                if (_acc_len > BITCHAT_MAX_PKT_LEN) {
                    _acc_len--;
                    memmove(_acc_buf, _acc_buf + 1, _acc_len);
                    skip_count++;
                    continue;
                }
                break;  // 정상적으로 더 많은 데이터 필요
            }

            // total_len이 비정상적으로 크면 가비지 → 1바이트 스킵
            if (v.total_len > BITCHAT_MAX_PKT_LEN) {
                _acc_len--;
                memmove(_acc_buf, _acc_buf + 1, _acc_len);
                skip_count++;
                continue;
            }

            if (v.total_len > _acc_len) {
                // 정상적인 불완전 패킷: 더 많은 데이터 대기
                // 단, 버퍼가 max 패킷보다 크면 가비지로 간주
                if (_acc_len > BITCHAT_MAX_PKT_LEN) {
                    _acc_len--;
                    memmove(_acc_buf, _acc_buf + 1, _acc_len);
                    skip_count++;
                    continue;
                }
                break;
            }

            // 완전한 패킷!
            if (skip_count > 0) {
                LOGD("BLE acc: skipped %d garbage bytes before packet", skip_count);
                skip_count = 0;
            }
            LOGD("BLE acc: complete packet %uB extracted", (unsigned)v.total_len);
            _r->enqueue_packet(_acc_buf, v.total_len, BC_SRC_SERVER_RX, 0xFFFF);

            size_t remaining = _acc_len - v.total_len;
            if (remaining > 0) {
                memmove(_acc_buf, _acc_buf + v.total_len, remaining);
            }
            _acc_len = remaining;
        }
        if (skip_count > 0) {
            LOGD("BLE acc: skipped %d garbage bytes (no complete packet found)", skip_count);
        }
    }
};

// ============================================================================
// Advertised device callbacks (scanner discovers bitchat peers)
// ============================================================================
class BitchatAdvertisedCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
    BitchatAdvertisedCallbacks(BitchatRelay* r) : _r(r) {}
    // NimBLE은 onResult가 pointer 인자로 전달
    void onResult(NimBLEAdvertisedDevice* dev) override {
        // Service UUID가 광고되어 있으면 bitchat 노드로 간주
        if (!dev->isAdvertisingService(NimBLEUUID(BC_BLE_SERVICE_UUID))) {
            return;
        }
        NimBLEAddress addr = dev->getAddress();
        // NimBLEAddress::getNative()는 const uint8_t* 직접 반환 (deref 불필요)
        const uint8_t* raw_addr = addr.getNative();

        // 블랙리스트 체크 - BLE 주소가 차단된 peer면 연결 시도 안 함
        if (blacklist_is_ble_blocked(raw_addr)) {
            LOGD("blacklist: skipping blocked ble peer %s",
                 std::string(addr).c_str());
            return;
        }

        LOGD("found bitchat peer: %s rssi=%d",
             std::string(addr).c_str(), dev->getRSSI());

        bool already_known = false;
        for (int i = 0; i < BC_MAX_OUT_CONN; ++i) {
            if (_r->_peers[i].connected &&
                _r->_peers[i].addr_valid &&
                memcmp(_r->_peers[i].addr, raw_addr, 6) == 0) {
                already_known = true;
                _r->_peers[i].last_seen_ms = millis();
                break;
            }
        }
        if (!already_known) {
            for (int i = 0; i < BC_MAX_OUT_CONN; ++i) {
                if (!_r->_peers[i].connected) {
                    memcpy(_r->_peers[i].addr, raw_addr, 6);
                    _r->_peers[i].addr_valid = true;
                    _r->_peers[i].last_seen_ms = millis();
                    _r->connect_to_peer(addr);
                    break;
                }
            }
        }
    }
private:
    BitchatRelay* _r;
};

// ============================================================================
// Client callbacks (outgoing connections)
// ============================================================================
class BitchatClientCallbacks : public NimBLEClientCallbacks {
public:
    BitchatClientCallbacks(BitchatRelay* r) : _r(r) {}
    void onConnect(NimBLEClient* c) override {
        LOGD("client connected: %s", std::string(c->getPeerAddress()).c_str());
    }
    void onDisconnect(NimBLEClient* c) override {
        LOGI("peer disconnected: %s",
             std::string(c->getPeerAddress()).c_str());
        for (int i = 0; i < BC_MAX_OUT_CONN; ++i) {
            if (_r->_peers[i].client == c) {
                _r->_peers[i].connected = false;
                _r->_peers[i].client = nullptr;
                
                _r->_peers[i].remote_char = nullptr;
                if (_r->_outgoing_count > 0) _r->_outgoing_count--;
                break;
            }
        }
    }
private:
    BitchatRelay* _r;
};

// ============================================================================
// BitchatRelay 구현
// ============================================================================
BitchatRelay::BitchatRelay()
    : _storage(nullptr), _server(nullptr), _service(nullptr),
      _char(nullptr), _advertising(nullptr),
      _scan(nullptr), _pkt_queue(nullptr), _scan_active(false),
      _incoming_count(0), _outgoing_count(0),
      _last_connect_ms(0),
      _peer_slot_count(0), _dedup_idx(0) {
    memset(_peers, 0, sizeof(_peers));
    memset(_dedup, 0, sizeof(_dedup));
}

BitchatRelay::~BitchatRelay() {
    if (_pkt_queue) vQueueDelete(_pkt_queue);
}

bool BitchatRelay::begin(bitchat_storage_t* storage) {
    _storage = storage;

    retry_init();
    blacklist_init();
    peer_keys_init();   // senderID → Ed25519 pk 매핑 로드 (서명 검증용)
    msg_cache_init();   // store-and-forward 캐시 초기화

    _pkt_queue = xQueueCreate(BC_PKT_QUEUE_LEN, sizeof(bitchat_pkt_event_t));
    if (!_pkt_queue) {
        LOGE("failed to create packet queue");
        return false;
    }

    // NimBLE 디바이스 초기화
    // BLE 이름은 항상 "bitchat" (고정)
    //   - 광고 패킷(31B)에 UUID(18B) + "bitchat"(9B) + flags(3B) = 30B ≤ 31B
    //   - 닉네임이 있으면 "bitchat-<nick>"가 되어 31B 초과 → 광고 실패
    //   - 닉네임은 SPIFFS(/node.conf)에 저장, Serial(node)에서만 표시
    NimBLEDevice::init("bitchat");
    NimBLEDevice::setMTU(247);  // NimBLE 기본 MTU 247 (Bluedroid 517과 다름)
    // NimBLE는 setMTU만으로 client와 server 모두에 적용됨

    // 1) GATT 서버
    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new BitchatServerCallbacks(this));
    _service = _server->createService(BC_BLE_SERVICE_UUID);

    // 단일 characteristic (write + notify) - 실제 bitchat 프로토콜 호환
    // bitchat 앱은 하나의 characteristic에 write + notify를 모두 사용
    _char = _service->createCharacteristic(
        BC_BLE_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
    );
    _char->setCallbacks(new BitchatRXCallbacks(this));
    _service->start();

    // 2) Advertising - 배터리 모드에서는 저듀티 (1.28s 간격)
    //    Service UUID를 광고에 포함 (bitchat 폰앱이 UUID로 릴레이 노드 발견)
    //    광고 패킷: flags(3) + UUID(18) + "bitchat"(9) = 30 ≤ 31 ✓
    _advertising = NimBLEDevice::getAdvertising();
    _advertising->addServiceUUID(BC_BLE_SERVICE_UUID);
    _advertising->setScanResponse(true);
#ifdef BC_POWER_BATTERY
    // setMinPreferred/setMaxPreferred는 0.625ms 단위 (Bluedroid과 동일)
    // 1.28s = 0x800, 2.048s = 0x1000
    _advertising->setMinPreferred(0x800);
    _advertising->setMaxPreferred(0x1000);
    LOGI("BLE advertising: low-duty 1.28-2.05s (battery mode, NimBLE)");
#else
    _advertising->setMinPreferred(0x06);
    _advertising->setMaxPreferred(0x12);
#endif
    _advertising->start();
    LOGI("BLE advertising started as bitchat (NimBLE, UUID=%s)", BC_BLE_SERVICE_UUID);

    // 3) Scanner - NimBLE는 ms 단위
    _scan = NimBLEDevice::getScan();
    _scan->setAdvertisedDeviceCallbacks(new BitchatAdvertisedCallbacks(this));
#ifdef BC_POWER_BATTERY
    _scan->setInterval(1280);  // 1.28s (ms 단위)
    _scan->setWindow(30);       // 30ms window
    LOGI("BLE scan: low-duty 30ms/1.28s (battery mode, NimBLE)");
#else
    _scan->setInterval(80);  // 80ms
    _scan->setWindow(40);   // 40ms
#endif
    _scan->setActiveScan(false);

    LOGI("BitchatRelay ready (NimBLE backend). max out=%d in=%d",
         BC_MAX_OUT_CONN, BC_MAX_IN_CONN);
    return true;
}

void BitchatRelay::enqueue_packet(const uint8_t* data, size_t len,
                                   BitchatPktSource src, uint16_t conn_id) {
    if (!data || len == 0 || len > BITCHAT_MAX_PKT_LEN) return;
    bitchat_pkt_event_t evt;
    memcpy(evt.data, data, len);
    evt.len = len;
    evt.source = src;
    evt.src_conn_id = conn_id;
    BaseType_t ok = xQueueSend(_pkt_queue, &evt, 0);
    if (ok != pdPASS) {
        LOGW("packet queue full, dropping %u bytes", (unsigned)len);
    }
}

void BitchatRelay::loop() {
    bitchat_pkt_event_t evt;
    while (xQueueReceive(_pkt_queue, &evt, 0) == pdPASS) {
        process_packet(&evt);
    }

    static uint32_t last_retry_check = 0;
    if (millis() - last_retry_check > 1000) {
        last_retry_check = millis();
        retry_check_timeouts(
            [](const uint8_t* packet, size_t len, void* user_data) {
                BitchatRelay* self = static_cast<BitchatRelay*>(user_data);
                self->forward_to_others(packet, len, 0xFFFE);
                power_led_pulse_packet();
            },
            this);
    }

    static uint32_t last_maint = 0;
    if (millis() - last_maint > 500) {
        last_maint = millis();
        maintain_connections();
        scan_cycle();
    }

    // Store-and-forward: 클라이언트 접속 시 캐시된 메시지 전송
    // 연결 후 2초 대기 → subscribe 완료 후 flush (데이터 유실 방지)
    static uint32_t last_flush_check = 0;
    if (millis() - last_flush_check > 500) {
        last_flush_check = millis();
        if (_incoming_count > 0 && _char && msg_cache_count() > 0 &&
            (millis() - _last_connect_ms > 2000)) {
            LOGI("store-and-forward: flushing %u cached messages", (unsigned)msg_cache_count());
            static NimBLECharacteristic* s_flush_char = nullptr;
            s_flush_char = _char;
            msg_cache_flush([](const uint8_t* d, size_t l) {
                if (s_flush_char) {
                    // BLE MTU 244B 제한: 큰 패킷은 청크 분할
                    if (l <= 244) {
                        s_flush_char->setValue(d, l);
                        s_flush_char->notify();
                    } else {
                        size_t off = 0;
                        while (off < l) {
                            size_t cl = (l - off > 244) ? 244 : (l - off);
                            s_flush_char->setValue(d + off, cl);
                            s_flush_char->notify();
                            off += cl;
                            if (off < l) delay(2);
                        }
                    }
                }
            });
            s_flush_char = nullptr;
        }
    }
}

void BitchatRelay::process_packet(bitchat_pkt_event_t* evt) {
    if (!evt) return;

    power_mark_active();
    power_led_pulse_packet();

    log_hexdump("RX", evt->data, evt->len);

    bitchat_packet_view_t v;
    if (!bitchat_parse(evt->data, evt->len, &v)) {
        LOGW("malformed packet from source=%u, ignoring", evt->source);
        return;
    }

    if (blacklist_is_pk_blocked(v.sender_id)) {
        char bl_id[BITCHAT_ID_STR_LEN];
        bitchat_id_from_pk(v.sender_id, bl_id);
        LOGW("blacklist: dropping packet from blocked sender=%s", bl_id);
        return;
    }

    // bitchat 프로토콜에 ACK 메커니즘 없음 - passive/explicit ACK 처리 제거
    // relay는 패킷을 받으면 그대로 forward만 함

    // ===== 서명 검증 + ANNOUNCE pk 추출 =====
    char id[BITCHAT_ID_STR_LEN];
    bitchat_id_from_pk(v.sender_id, id);
    LOGI("received packet from sender=%s type=0x%02x", id, v.type);

    // ANNOUNCE(0x01) 처리: payload에서 32B Ed25519 공개키 추출 → 매핑 저장
    if (v.type == BITCHAT_MSG_ANNOUNCE && v.payload && v.payload_len >= 10) {
        // ANNOUNCE payload는 TLV 포맷:
        //   [01 05 "nickname"] [02 20 <32B key A>] [03 20 <32B key B>] ...
        // type 0x02 또는 0x03이 Ed25519 서명용 공개키일 수 있음
        // → 두 키 모두 추출하여 검증 시도
        size_t off = 0;
        uint8_t pk_a[32] = {0};  // type 0x02
        uint8_t pk_b[32] = {0};  // type 0x03
        bool found_a = false, found_b = false;
        while (off + 2 <= v.payload_len) {
            uint8_t ftype = v.payload[off];
            uint8_t flen  = v.payload[off + 1];
            if (off + 2 + flen > v.payload_len) break;
            if (flen == 32) {
                if (ftype == 0x02) {
                    memcpy(pk_a, v.payload + off + 2, 32);
                    found_a = true;
                } else if (ftype == 0x03) {
                    memcpy(pk_b, v.payload + off + 2, 32);
                    found_b = true;
                }
            }
            off += 2 + flen;
        }
        // type 0x02 → key index 0, type 0x03 → key index 1
        if (found_a) {
            peer_keys_add_indexed(v.sender_id, pk_a, 0);
            LOGI("ANNOUNCE: key A (type=0x02) stored at index 0 (%02x%02x%02x%02x...)",
                 pk_a[0], pk_a[1], pk_a[2], pk_a[3]);
        }
        if (found_b) {
            peer_keys_add_indexed(v.sender_id, pk_b, 1);
            LOGI("ANNOUNCE: key B (type=0x03) stored at index 1 (%02x%02x%02x%02x...)",
                 pk_b[0], pk_b[1], pk_b[2], pk_b[3]);
        }
    }

    // 서명 검증: 두 키(index 0, 1) 모두 시도
    if (v.signature) {
        uint8_t sign_msg[BITCHAT_MAX_PKT_LEN];
        size_t sign_msg_len = bitchat_compute_signing_message(
            evt->data, evt->len, &v, sign_msg, sizeof(sign_msg));
        if (sign_msg_len > 0) {
            bool verified = false;
            for (int ki = 0; ki < 2 && !verified; ++ki) {
                uint8_t pk[32];
                if (peer_keys_get_indexed(v.sender_id, ki, pk)) {
                    if (crypto_verify_signature(pk, 32,
                                                sign_msg, sign_msg_len,
                                                v.signature, 64)) {
                        LOGI("[OK] signature verified (key index %d) from %s", ki, id);
                        verified = true;
                    } else {
                        LOGD("signature check with key %d failed", ki);
                    }
                }
            }
            if (!verified) {
                LOGW("[!] signature failed with all keys from %s (forwarding anyway)", id);
            }
        }
    }

    // Dedup: use parsed payload pointer (not raw offset) for accurate chunk distinction
    // v.payload points to actual payload content (after header + sender_id + recipient_id)
    // This correctly distinguishes image chunks that share the same sender_id + timestamp
    if (check_dedup(v.sender_id, v.timestamp,
                    reinterpret_cast<const uint8_t*>(v.payload), v.payload_len)) {
        LOGD("duplicate packet, dropping");
        return;
    }

    if (_storage) storage_add_peer(_storage, v.sender_id);

    if (v.ttl == 0) {
        LOGD("TTL=0, not forwarding further");
        return;
    }

    uint8_t fwd_buf[BITCHAT_MAX_PKT_LEN];
    size_t fwd_len = bitchat_decrement_ttl(evt->data, evt->len,
                                            fwd_buf, sizeof(fwd_buf));
    if (fwd_len == 0) {
        LOGE("TTL decrement failed");
        return;
    }
    forward_to_others(fwd_buf, fwd_len, evt->src_conn_id);
    LOGI("forwarded pkt (ttl %u->%u) to other peers",
         v.ttl, (uint8_t)(v.ttl - 1));

    // 개선사항: MESSAGE(0x02) 타입만 retry 추적
    // ANNOUNCE(0x01), LEAVE(0x03), SYNC(0x21), NOISE(0x10/0x11) 등은 추적 안 함
    // → 불필요한 재전송 방지, 대역폭 절약
    if (v.type == BITCHAT_MSG_MESSAGE) {
        retry_enqueue(fwd_buf, fwd_len);
    }
}

bool BitchatRelay::verify_packet(const uint8_t* data, size_t len,
                                  bitchat_packet_view_t* view) {
    // 서명 검증 일단 스킵 - senderID가 8B이므로 pk 매핑 필요
    // TODO: ANNOUNCE 메시지에서 pk 추출 후 storage에 매핑
    return bitchat_parse(data, len, view);
}

bool BitchatRelay::check_dedup(const uint8_t* sender_pk, uint64_t counter,
                                const uint8_t* payload, size_t payload_len) {
    // key = sender_id(8) + timestamp(4) + payload_hash(4) = 16B
    // payload_hash: 실제 payload의 첫 4바이트를 사용하여 이미지 청크 등
    // 같은 timestamp를 공유하는 패킷도 구분 가능
    // (이전: raw packet offset 22 사용 → recipient_id(ffffffff)로 모두 동일했음)
    uint8_t key[16];
    memcpy(key, sender_pk, 8);
    for (int i = 0; i < 8; ++i) key[8 + i] = (counter >> (8*(7-i))) & 0xFF;

    // payload 전체를 XOR 해시하여 4바이트 키 생성
    // (이전: payload[0..3]만 사용 → 이미지 청크들이 앞부분이 같아 구분 불가)
    // XOR 해시는 payload의 모든 바이트를 반영하므로 시퀀스 번호 등 차이를 감지
    if (payload && payload_len > 0) {
        uint8_t hash[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < payload_len; ++i) {
            hash[i & 3] ^= payload[i];
        }
        memcpy(key + 12, hash, 4);
    }

    uint32_t now = millis();
    for (int i = 0; i < BC_DEDUP_CACHE_SIZE; ++i) {
        if (!_dedup[i].used) continue;
        if (now - _dedup[i].ts > 30000) {
            _dedup[i].used = false;
            continue;
        }
        if (memcmp(_dedup[i].key, key, 16) == 0) return true;
    }
    _dedup[_dedup_idx].used = true;
    _dedup[_dedup_idx].ts = now;
    memcpy(_dedup[_dedup_idx].key, key, 16);
    _dedup_idx = (_dedup_idx + 1) % BC_DEDUP_CACHE_SIZE;
    return false;
}

void BitchatRelay::forward_to_others(const uint8_t* data, size_t len,
                                      uint16_t except_conn) {
    // 0) LoRa로도 브로드캐스트 (BLE↔LoRa 브릿지)
    //    BLE에서 들어온 패킷을 LoRa로도 송신 → 장거리 메시 확장
    if (lora_relay_is_detected()) {
        lora_relay_broadcast(data, len);
    }

    // 0.5) WiFi/MQTT로도 브로드캐스트 (BLE↔인터넷 브릿지)
    //      Serial 'wifi on'으로 활성화된 경우에만 동작
    if (wifi_bridge_is_connected()) {
        wifi_bridge_publish(data, len);
    }

    // 1) 모든 incoming clients에게 notify
    //    BLE MTU 제한 (244 bytes) 때문에 큰 패킷은 청크로 분할 전송
    //    수신측(bitchat 앱)이 BLE notify를 누적하여 재조립해야 함
    if (_char) {
        if (len <= 244) {
            // 작은 패킷은 그대로 notify
            _char->setValue(data, len);
            _char->notify();
        } else {
            // 큰 패킷은 244바이트 청크로 분할
            size_t offset = 0;
            while (offset < len) {
                size_t chunk_len = (len - offset > 244) ? 244 : (len - offset);
                _char->setValue(data + offset, chunk_len);
                _char->notify();
                offset += chunk_len;
                // 청크 간 미세 delay (BLE 스택이 처리할 시간 확보)
                if (offset < len) delay(2);
            }
            LOGD("forwarded %uB in %u chunks (BLE notify split)", (unsigned)len,
                 (unsigned)((len + 243) / 244));
        }
    }
    // Store-and-forward: 연결된 클라이언트가 없으면 MESSAGE 캐시에 저장
    if (_incoming_count == 0) {
        bitchat_packet_view_t cv;
        if (bitchat_parse(data, len, &cv) && cv.type == BITCHAT_MSG_MESSAGE) {
            msg_cache_add(data, len);
        }
    }

    // 2) outgoing peers의 RX char에 write
    for (int i = 0; i < BC_MAX_OUT_CONN; ++i) {
        PeerEntry* p = &_peers[i];
        if (!p->connected) continue;
        forward_to_peer(p, data, len);
    }
}

void BitchatRelay::forward_to_peer(PeerEntry* p, const uint8_t* data, size_t len) {
    if (!p || !p->connected || !p->remote_char) return;
    try {
        // NimBLE writeValue: bool response 인자 (기본 true, write-with-response)
        std::string val(reinterpret_cast<const char*>(data), len);
        p->remote_char->writeValue(val, true);
        p->pkts_forwarded++;
    } catch (...) {
        LOGW("write to peer failed, marking disconnected");
        disconnect_peer(p);
    }
}

void BitchatRelay::scan_cycle() {
    static uint32_t scan_start = 0;
    static uint32_t cycle_start = 0;
    uint32_t now = millis();

#ifdef BC_POWER_BATTERY
    const uint32_t SCAN_DURATION = 2000;
    const uint32_t CYCLE_GAP     = 30000;
#else
    const uint32_t SCAN_DURATION = 5000;
    const uint32_t CYCLE_GAP     = 500;
#endif
    if (cycle_start == 0) cycle_start = now;

    if (_scan_active && (now - scan_start > SCAN_DURATION)) {
        _scan->stop();
        _scan_active = false;
        cycle_start = now;
        LOGT("scan stopped after %ums", (unsigned)SCAN_DURATION);
        return;
    }

    if (!_scan_active && (now - cycle_start > CYCLE_GAP - SCAN_DURATION)) {
        _scan_active = true;
        scan_start = now;
        // NimBLE Scan::start(duration, callback, is_continue)
        // duration=0 means continuous (we stop manually)
        _scan->start(0, nullptr, false);
        LOGT("scan started");
    }
}

void BitchatRelay::maintain_connections() {
    uint32_t now = millis();
    for (int i = 0; i < BC_MAX_OUT_CONN; ++i) {
        PeerEntry* p = &_peers[i];
        if (!p->connected) continue;
        if (now - p->last_seen_ms > 5 * 60 * 1000) {
            NimBLEAddress tmp_addr(p->addr[0] | (uint64_t)p->addr[1] << 8 |
                                    (uint64_t)p->addr[2] << 16 |
                                    (uint64_t)p->addr[3] << 24 |
                                    (uint64_t)p->addr[4] << 32 |
                                    (uint64_t)p->addr[5] << 40);
            LOGI("peer idle 5min, disconnecting %s",
                 std::string(tmp_addr).c_str());
            disconnect_peer(p);
        }
    }
}

bool BitchatRelay::connect_to_peer(NimBLEAddress addr) {
    int slot = -1;
    for (int i = 0; i < BC_MAX_OUT_CONN; ++i) {
        if (!_peers[i].connected) { slot = i; break; }
    }
    if (slot < 0) {
        LOGD("no free outgoing slot for %s",
             std::string(addr).c_str());
        return false;
    }
    PeerEntry* p = &_peers[slot];
    memcpy(p->addr, addr.getNative(), 6);
    p->addr_valid = true;

    NimBLEClient* client = NimBLEDevice::createClient();
    client->setClientCallbacks(new BitchatClientCallbacks(this));
    LOGI("connecting to peer %s ...", std::string(addr).c_str());
    if (!client->connect(addr)) {
        LOGW("connect failed to %s", std::string(addr).c_str());
        NimBLEDevice::deleteClient(client);
        return false;
    }
    NimBLERemoteService* svc = client->getService(BC_BLE_SERVICE_UUID);
    if (!svc) {
        LOGW("peer %s missing bitchat service", std::string(addr).c_str());
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return false;
    }
    p->remote_char = svc->getCharacteristic(BC_BLE_CHAR_UUID);
    if (!p->remote_char) {
        LOGW("peer %s missing bitchat char", std::string(addr).c_str());
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return false;
    }
    // notify 구독 - NimBLE는 subscribe(true) 호출
    if (p->remote_char->canNotify()) {
        p->remote_char->subscribe(true,
            [](NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
                if (!isNotify || !data || len == 0) return;
                g_relay.enqueue_packet(data, len, BC_SRC_CLIENT_NOTIFY, 0xFFFE);
            });
    } else {
        LOGW("peer %s tx char doesn't support notify",
             std::string(addr).c_str());
    }

    p->client = client;
    p->connected = true;
    p->last_seen_ms = millis();
    _outgoing_count++;
    LOGI("connected to peer %s (out=%d, NimBLE)",
         std::string(addr).c_str(), _outgoing_count);
    return true;
}

void BitchatRelay::disconnect_peer(PeerEntry* p) {
    if (!p || !p->connected) return;
    p->connected = false;
    if (p->client) {
        p->client->disconnect();
        NimBLEDevice::deleteClient(p->client);
        p->client = nullptr;
    }
    
    p->remote_char = nullptr;
    if (_outgoing_count > 0) _outgoing_count--;
    NimBLEAddress tmp_addr(p->addr[0] | (uint64_t)p->addr[1] << 8 |
                            (uint64_t)p->addr[2] << 16 |
                            (uint64_t)p->addr[3] << 24 |
                            (uint64_t)p->addr[4] << 32 |
                            (uint64_t)p->addr[5] << 40);
    LOGI("disconnected peer %s (out=%d)",
         std::string(tmp_addr).c_str(), _outgoing_count);
    p->addr_valid = false;
}
