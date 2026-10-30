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
    // === v10 핵심 수정 ===
    // NimBLE 1.4.2는 onConnect(server)와 onConnect(server, desc)를 "둘 다"
    // 호출한다. 기존은 단일 인자 버전만 override했기 때문에 피어 주소를
    // 알 수 없었다. v10은 desc 버전만 override하여 피어 주소를 추적한다
    // (단일 인자 버전은 기본 no-op 구현이 호출되므로 중복 카운트 없음).
    void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
        // 클라이언트(앱)가 새로 연결되면 accumulator를 무조건 리셋.
        // 앱을 닫았다 다시 열면(재연결) 이전 세션의 이미지 잔여물이
        // "가짜 헤더"(선언된 total_len보다 데이터가 모자란 상태)로
        // 버퍼에 남아 있고, 새 세션의 단문이 그 뒤에 쌓여도 영원히
        // 추출되지 않았음. 연결 경계 = 새 세션 시작 → 버퍼 완전 초기화.
        _r->ble_acc_reset("connect");
        _r->add_server_peer(NimBLEAddress(desc->peer_id_addr));
        // 폰이 서버로 접속했으면 대기 중인 outgoing 연결 시도는 취소
        // (같은 디바이스에 2중 연결 시도 → 컨트롤러 충돌 방지)
        _r->_pending_valid = false;
        _r->_incoming_count++;
        _r->_last_connect_ms = millis();   // store-and-forward flush 2초 지연용
        LOGI("client connected (%s), in=%d out=%d",
             std::string(NimBLEAddress(desc->peer_id_addr)).c_str(),
             _r->_incoming_count, _r->_outgoing_count);
    }
    void onDisconnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
        // 연결 종료 = 세션 종료 → 대기 중이던 불완전 패킷은 절대 완성 안 됨.
        // 리셋하지 않으면 재접속 시 가짜 헤더가 남아 증상이 재현됨.
        _r->ble_acc_reset("disconnect");
        _r->remove_server_peer(NimBLEAddress(desc->peer_id_addr));
        if (_r->_incoming_count > 0) _r->_incoming_count--;
        LOGI("client disconnected (%s), in=%d",
             std::string(NimBLEAddress(desc->peer_id_addr)).c_str(),
             _r->_incoming_count);
        // advertise 재시작 (NimBLE은 자동으로 안 함)
        // 실패하더라도 loop()의 광고 자가 복구(2초 주기)가 재시도하므로
        // 폰이 영영 릴레이를 못 찾는 상황은 발생하지 않음
        if (!server->getAdvertising()->start()) {
            LOGW("adv restart on disconnect failed, self-heal will retry");
        }
    }
private:
    BitchatRelay* _r;
};

// ============================================================================
// RX characteristic callbacks (clients write to us)
// ============================================================================
// BLE write accumulator: 큰 패킷(이미지 등)이 여러 BLE write로 분할되어
// 전송되는 것을 누적하여 완전한 패킷으로 조립 후 처리.
// v9: 버퍼/상태를 BitchatRelay 멤버로 승격 → 서버 콜백(onConnect/
// onDisconnect)에서 연결 경계마다 리셋 가능 (앱 재시작 버그 수정).

// accumulator 버퍼 크기: 2패킷 분량 + 마진 (ble_relay.h의 _acc_buf와 동일)
#define BLE_ACC_BUF_SIZE  (2 * BITCHAT_MAX_PKT_LEN + 128)  // 2*808 + 128 = 1744B

// 주어진 type 바이트가 bitchat 프로토콜의 유효한 메시지 타입인지 확인
// → 가비지 바이트가 우연히 0x01(version)과 일치해도 type 필드로
//   정상 패킷인지 가비지인지 구분 가능
static bool is_valid_bitchat_type(uint8_t t) {
    return t == BITCHAT_MSG_ANNOUNCE        // 0x01
        || t == BITCHAT_MSG_MESSAGE         // 0x02
        || t == BITCHAT_MSG_LEAVE           // 0x03
        || t == BITCHAT_MSG_NOISE_HANDSHAKE // 0x10
        || t == BITCHAT_MSG_NOISE_ENCRYPTED // 0x11
        || t == BITCHAT_MSG_FRAGMENT        // 0x20
        || t == BITCHAT_MSG_REQUEST_SYNC    // 0x21
        || t == BITCHAT_MSG_FILE_TRANSFER   // 0x22
        || t == BITCHAT_MSG_VOICE_FRAME;    // 0x29
}

// write 간격 감시 임계값.
// 정상 bitchat 패킷의 BLE write 분할은 burst(30~90ms 간격)로 도착함.
// 이전 write와 1.5초 이상 떨어진 onWrite가 도착했는데도 버퍼에
// 불완전 패킷이 대기 중이라면, 그 head는 전송이 중단된 가짜 헤더
// (이미지 꼬리의 시그니처 바이트가 우연히 패킷 헤더처럼 보이는 것)임.
#define BLE_ACC_GAP_TIMEOUT_MS   1500

class BitchatRXCallbacks : public NimBLECharacteristicCallbacks {
public:
    BitchatRXCallbacks(BitchatRelay* r) : _r(r) {}

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

        uint32_t now = millis();

        // === v9: write 간격 감시 → resync 모드 ===
        // 버퍼에 대기 중인 불완전 패킷이 있는 상태로 1.5초 이상 write가
        // 끊겼다면 그 패킷의 나머지 분할은 오지 않은 것(전송 중단/가짜
        // 헤더). v8은 이때 버퍼 전체를 버렸지만, 대기 head 뒤에 이미
        // 쌓여 있던 정상 패킷까지 같이 사라지는 문제가 있었음.
        // v9는 resync 모드로 전환: extract_packets()가 대기하지 않고
        // 가짜 헤더를 1바이트씩 건너뛰어 뒤의 정상 패킷을 "구출"함.
        if (_r->_acc_len > 0 &&
            (uint32_t)(now - _r->_acc_last_write_ms) > BLE_ACC_GAP_TIMEOUT_MS) {
            LOGW("BLE acc: write gap %ums with %uB pending -> resync mode",
                 (unsigned)(now - _r->_acc_last_write_ms),
                 (unsigned)_r->_acc_len);
            _r->_acc_resync = true;
        }
        _r->_acc_last_write_ms = now;

        // 0) 새 데이터 추가 전에 기존 버퍼에서 완전한 패킷 추출 (공간 확보)
        //    이 단계가 없으면 2개 패킷이 버퍼에 쌓였을 때 overflow 발생
        _r->ble_extract_packets();

        // 1) 새 데이터 추가
        if (_r->_acc_len + len > BLE_ACC_BUF_SIZE) {
            // 추출 후에도 공간 부족 → 버퍼 리셋 (비정상 상황)
            LOGW("BLE acc: overflow (%u+%u > %u) after extraction, resetting",
                 (unsigned)_r->_acc_len, (unsigned)len, BLE_ACC_BUF_SIZE);
            _r->_acc_len = 0;
        }
        memcpy(_r->_acc_buf + _r->_acc_len, data, len);
        _r->_acc_len += len;

        // 2) 새 데이터 추가 후 다시 추출 시도
        _r->ble_extract_packets();
    }

private:
    BitchatRelay* _r;
};

// ============================================================================
// BLE accumulator 구현 (v9) - BitchatRelay 멤버
// ============================================================================

// 연결 경계(onConnect/onDisconnect) 및 비정상 상태에서 accumulator를 리셋.
// 연결이 새로 맺어지거나 끊어지면 이전 세션의 불완전 데이터는 절대
// 완성되지 않으므로 가짜 헤더가 남을 이유가 없음 → 완전 초기화.
void BitchatRelay::ble_acc_reset(const char* reason) {
    if (_acc_len > 0) {
        LOGI("BLE acc: reset on %s (%u bytes pending discarded)",
             reason, (unsigned)_acc_len);
    }
    _acc_len = 0;
    _acc_wait_total = 0;
    _acc_resync = false;
    _acc_last_write_ms = 0;
}

// 누적 버퍼에서 완전한 패킷 추출 (0개 이상 추출 가능)
//
// 가짜 헤더(가비지가 우연히 version=0x01 + 유효 type + 합리적 payload_len
// 형태를 띤 것) 판정은 3중 방어:
//   1. 연결 경계 리셋  - onConnect/onDisconnect에서 버퍼 완전 초기화 (결정적)
//   2. resync 모드     - write 간격 >1.5초인데 대기 중이면 가짜로 판정해
//                        1바이트씩 건너뛰며 뒤의 정상 패킷 구출 (시간 기반)
//   3. total_len 초과  - head가 선언한 total_len을 초과했는데도 파싱이
//                        안 되면 head는 가짜 → 1바이트 스킵 (결정적)
// 정상 불완전 패킷은 BLE burst 내에 완성되므로 2번의 1.5초 임계값에
// 걸리지 않음. 3번은 수학적으로 참(진짜 패킷은 자기 total_len을 넘을 수
// 없음)이므로 오판 없음.
void BitchatRelay::ble_extract_packets() {
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
            // 파싱 실패. 두 가지 가능성:
            //   (A) 정상적인 불완전 패킷 - 더 많은 데이터 필요 → 대기
            //   (B) 가비지 - byte 0가 우연히 0x01이지만 나머지는 쓰레기 → 스킵
            uint8_t pkt_type = _acc_buf[1];
            bool type_valid = is_valid_bitchat_type(pkt_type);

            // === v9 방어선 3: total_len 초과 판정 ===
            // 이전에 parse가 성공해서 head의 선언 길이를 기록해 뒀는데
            // 버퍼가 그 길이를 초과했음에도 아직 파싱 실패라면, head는
            // 가짜 헤더임이 확실(진짜 패킷은 선언 길이에서 반드시 완성).
            // 대기 대신 스킵해서 뒤에 쌓인 정상 패킷을 구출.
            if (_acc_wait_total > 0 && _acc_len > _acc_wait_total) {
                if (skip_count == 0) {
                    LOGW("BLE acc: fake header detected "
                         "(declared %uB, buffer %uB) -> resync",
                         (unsigned)_acc_wait_total, (unsigned)_acc_len);
                }
                _acc_wait_total = 0;
                _acc_len--;
                memmove(_acc_buf, _acc_buf + 1, _acc_len);
                skip_count++;
                continue;
            }

            if (type_valid && _acc_len <= BITCHAT_MAX_PKT_LEN && !_acc_resync) {
                // 정상적인 불완전 패킷 - 더 많은 데이터 대기
                break;
            }
            // resync 모드면 대기하지 않고 가짜 헤더를 건너뜀
            _acc_len--;
            memmove(_acc_buf, _acc_buf + 1, _acc_len);
            skip_count++;
            continue;
        }

        // total_len이 비정상적으로 크면 가비지 → 1바이트 스킵
        if (v.total_len > BITCHAT_MAX_PKT_LEN) {
            _acc_len--;
            memmove(_acc_buf, _acc_buf + 1, _acc_len);
            skip_count++;
            continue;
        }

        if (v.total_len > _acc_len) {
            // 파싱은 성공했으나 아직 전체 패킷이 도착하지 않음.
            // 정상이면 대기하지만, resync 모드(write 간격 초과)에서는
            // 전송이 이미 중단된 가짜 헤더로 간주하고 건너뜀.
            if (!_acc_resync) {
                _acc_wait_total = v.total_len;   // 기대 길이 기록 (방어선 3용)
                break;
            }
            _acc_len--;
            memmove(_acc_buf, _acc_buf + 1, _acc_len);
            skip_count++;
            continue;
        }

        // 완전한 패킷!
        _acc_wait_total = 0;
        if (skip_count > 0) {
            LOGD("BLE acc: skipped %d garbage bytes before packet", skip_count);
            skip_count = 0;
        }
        LOGD("BLE acc: complete packet %uB extracted", (unsigned)v.total_len);
        enqueue_packet(_acc_buf, v.total_len, BC_SRC_SERVER_RX, 0xFFFF);

        size_t remaining = _acc_len - v.total_len;
        if (remaining > 0) {
            memmove(_acc_buf, _acc_buf + v.total_len, remaining);
        }
        _acc_len = remaining;
        // 정상 패킷은 최소 MIN_PKT_LEN(22B)이므로, 추출 후 남은 잔여물이
        // 22바이트 미만이면 무조건 가비지로 판단하여 버림.
        // → 다음 단문 패킷이 버퍼 맨 앞에서 깨끗하게 시작됨
        if (_acc_len > 0 && _acc_len < BITCHAT_MIN_PKT_LEN) {
            LOGD("BLE acc: discarded %d small leftover bytes", (int)_acc_len);
            _acc_len = 0;
        }
    }
    if (skip_count > 0) {
        LOGD("BLE acc: skipped %d garbage bytes (no complete packet found)", skip_count);
    }
    // while 루프 종료 후 (break로 빠져나온 경우) 남은 잔여물 처리
    // 정상 패킷은 최소 22바이트이므로 22바이트 미만 잔여물은 가비지.
    if (_acc_len > 0 && _acc_len < BITCHAT_MIN_PKT_LEN) {
        LOGD("BLE acc: discarded %d small leftover bytes (post-loop)", (int)_acc_len);
        _acc_len = 0;
    }
    // resync 모드는 추출 시도 1회 동안만 유지.
    // 이번에 끝까지 스킵했는데도 여전히 "대기" 상태로 끝났다면 그 head는
    // 다음 write가 오면 정상 대기 로직으로 판정함(1.5초 간격 감시가 재판정).
    _acc_resync = false;
}

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
            // 이미 서버(수신 링크)로 연결된 디바이스면 outgoing 연결 시도 금지.
            // 폰 앱 재실행 시 폰이 우리 서버로 접속하는데, 스캔이 폰 광고를
            // 발견해서 outgoing 연결을 또 시도하면 컨트롤러 충돌로
            // 재접속이 반복 실패할 수 있음.
            if (_r->is_server_peer(addr)) {
                return;
            }
            // === v10 핵심 수정 ===
            // onResult는 NimBLE 호스트 태스크(ble host task)에서 호출된다.
            // 여기서 블로킹 connect()를 직접 호출하면:
            //   1) connect 완료 이벤트를 처리할 주체가 지금 이 태스크 자신인데
            //      콜백 안에서 세마포어를 기다리므로 타임아웃(3+1초)까지
            //      BLE 전체(서버 수신 포함)가 멈춤 = 4초 블랙아웃
            //   2) 대기 중에 LL 연결이 맺어지면 타임아웃 후 NimBLE이
            //      m_pTaskData==nullptr를 보고 방금 맺은 연결을 강제 종료
            //      (NimBLEClient.cpp의 BLE_GAP_EVENT_CONNECT 참고)
            //      → 폰 재접속이 반복 실패 → 앱 재실행 후 덤프가 안 나옴
            // → 여기서는 주소만 기록하고 실제 연결은 loop() 태스크에서 수행
            if (!_r->_pending_valid && !_r->_pending_in_flight) {
                _r->_pending_address = addr;
                _r->_pending_valid = true;
                _r->_pending_seen_ms = millis();
                LOGD("outgoing connect queued: %s",
                     std::string(addr).c_str());
            }
            (void)raw_addr;
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
      _srv_peer_idx(0),
      _pending_valid(false), _pending_in_flight(false),
      _pending_seen_ms(0), _pending_last_try_ms(0),
      _peer_slot_count(0), _dedup_idx(0) {
    memset(_peers, 0, sizeof(_peers));
    memset(_dedup, 0, sizeof(_dedup));
    // v9: BLE accumulator 초기화
    memset(_acc_buf, 0, sizeof(_acc_buf));
    _acc_len = 0;
    _acc_last_write_ms = 0;
    _acc_wait_total = 0;
    _acc_resync = false;
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

    // === v10: 지연된 outgoing 연결 처리 (아두이노 태스크에서 안전하게) ===
    process_pending_connect();

    // === v10: 광고 자가 복구 ===
    // onDisconnect에서 광고 재시작이 실패했거나 다른 이유로 광고가 꺼져
    // 있으면 폰이 릴레이를 영영 찾지 못함(앱 재실행 후 덤프 없음) →
    // 2초마다 점검하여 클라이언트가 없는데 광고가 꺼져 있으면 재시작
    static uint32_t last_adv_check = 0;
    if (millis() - last_adv_check > 2000) {
        last_adv_check = millis();
        if (_incoming_count == 0 && _advertising &&
            !_advertising->isAdvertising()) {
            LOGW("advertising inactive with no client, restarting (self-heal)");
            _advertising->start();
        }
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
        // 폰(incoming client)이 연결되어 있으면 scan 금지
        // → 스캔이 다른 bitchat 노드를 발견하고 연결 시도하면
        //   BLE 컨트롤러가 30초간 점유되어 폰의 단문 수신이 막힘
        // 폰이 연결된 상태에서는 폰 통신을 최우선으로 보장
        if (_incoming_count > 0) {
            // scan 중이면 중지
            if (_scan_active) {
                _scan->stop();
                _scan_active = false;
            }
        } else {
            scan_cycle();
        }
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
    // dedup key = 16 bytes:
    //   key[0..7]   = sender_id (8 bytes)
    //   key[8..11]  = timestamp 상위 4 bytes
    //   key[12..15] = timestamp 하위 4 bytes XOR payload_hash(4 bytes)
    //
    // payload_hash는 FNV-1a 32-bit 해시를 사용.
    // XOR hash 대신 FNV-1a를 쓰는 이유:
    //   - XOR hash는 같은 패턴이 반복되면 충돌 (예: "AAAA"와 "BBBB"는 다르지만
    //     4바이트 XOR hash는 둘 다 같은 위치에 같은 길이의 패턴 → 해시가 우연히 같을 수 있음)
    //   - FNV-1a는 각 바이트를 해시에 반영하면서 곱셈으로 확산시키므로 충돌 확률이 훨씬 낮음
    //
    // timestamp 하위 4 bytes와 XOR 하는 이유:
    //   - 이전 코드는 memcpy(key+12, hash, 4)로 timestamp 하위 4 bytes를 덮어씀
    //   - 같은 sender가 같은 내용의 메시지(예: "test")를 연속으로 보내면
    //     timestamp 상위 4 bytes는 같고(같은 초 범위) payload_hash도 같아서
    //     모든 메시지가 같은 key로 dedup 됨 → 2번째부터 "duplicate"로 drop됨
    //   - XOR 하면 timestamp 하위 4 bytes의 차이가 key에 반영됨
    uint8_t key[16];
    memcpy(key, sender_pk, 8);
    for (int i = 0; i < 8; ++i) key[8 + i] = (counter >> (8*(7-i))) & 0xFF;

    // FNV-1a 32-bit hash of payload
    if (payload && payload_len > 0) {
        uint32_t h = 0x811c9dc5;  // FNV offset basis
        for (size_t i = 0; i < payload_len; ++i) {
            h ^= payload[i];
            h *= 0x01000193;       // FNV prime
        }
        // XOR with timestamp low 4 bytes (key[12..15])
        key[12] ^= (uint8_t)((h >> 24) & 0xFF);
        key[13] ^= (uint8_t)((h >> 16) & 0xFF);
        key[14] ^= (uint8_t)((h >>  8) & 0xFF);
        key[15] ^= (uint8_t)((h      ) & 0xFF);
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

    if (!_scan_active && !_pending_in_flight &&
        (now - cycle_start > CYCLE_GAP - SCAN_DURATION)) {
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
    // 연결 타임아웃 3초로 단축 (기본 30초 → 3초)
    // 긴 타임아웃은 BLE 컨트롤러를 점유하여 폰의 단문 수신을 30초간 막음
    client->setConnectTimeout(3);    // 3 seconds (NimBLE 1.4.2: uint8_t 단위 = 초)
    LOGI("connecting to peer %s (timeout=3s) ...", std::string(addr).c_str());
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

// ============================================================================
// 서버(incoming) 피어 주소 추적 (v10)
// ============================================================================
// 서버로 연결된 피어 주소를 기록/삭제/조회한다.
// 용도: 스캔이 발견한 주소가 이미 서버 링크로 연결된 디바이스(폰)면
// outgoing 연결을 시도하지 않도록 차단.
bool BitchatRelay::is_server_peer(const NimBLEAddress& addr) {
    for (int i = 0; i < BC_MAX_IN_CONN; ++i) {
        if (_srv_peers[i] == addr) return true;
    }
    return false;
}

void BitchatRelay::add_server_peer(const NimBLEAddress& addr) {
    // 이미 기록돼 있으면 스킵
    for (int i = 0; i < BC_MAX_IN_CONN; ++i) {
        if (_srv_peers[i] == addr) return;
    }
    // 빈 슬롯 우선 사용
    for (int i = 0; i < BC_MAX_IN_CONN; ++i) {
        if (_srv_peers[i] == NimBLEAddress()) {
            _srv_peers[i] = addr;
            return;
        }
    }
    // 모든 슬롯 사용 중 → 순환 인덱스로 가장 오래된 슬롯 교체
    _srv_peers[_srv_peer_idx % BC_MAX_IN_CONN] = addr;
    _srv_peer_idx = (_srv_peer_idx + 1) % BC_MAX_IN_CONN;
}

void BitchatRelay::remove_server_peer(const NimBLEAddress& addr) {
    for (int i = 0; i < BC_MAX_IN_CONN; ++i) {
        if (_srv_peers[i] == addr) {
            _srv_peers[i] = NimBLEAddress();
            return;
        }
    }
}

// ============================================================================
// 지연 연결 처리 (v10) - 반드시 loop()(아두이노 태스크)에서만 호출
// ============================================================================
// onResult(호스트 태스크)에서 connect()를 직접 호출하면 호스트 태스크가
// 자기 완료 이벤트를 기다리는 자기교착(4초 블랙아웃 + 강제 terminate)이
// 발생하므로, 발견한 주소를 여기서 안전하게 연결한다.
void BitchatRelay::process_pending_connect() {
    if (!_pending_valid || _pending_in_flight) return;
    if (_incoming_count > 0) {
        // 폰이 서버로 접속했다면 outgoing 연결 불필요
        // (단일 링크로 write/notify 모두 처리 가능)
        _pending_valid = false;
        return;
    }
    uint32_t now = millis();
    // 발견 후 2초 유예: 폰이 먼저 우리 서버로 접속할 시간을 줌
    if (now - _pending_seen_ms < 2000) return;
    // 같은 주소 재시도 쿨다운 10초 (무한 재시도 스팸 방지)
    if (_pending_last_try_ms != 0 &&
        now - _pending_last_try_ms < 10000) return;

    // 스캔 중이면 중지 후 시도 (BLE 컨트롤러 충돌 방지)
    if (_scan_active) {
        _scan->stop();
        _scan_active = false;
    }

    _pending_in_flight = true;
    _pending_last_try_ms = now;
    LOGI("deferred connect attempt: %s",
         std::string(_pending_address).c_str());
    connect_to_peer(_pending_address);
    _pending_in_flight = false;
    // 성공/실패 무관 소비. 실패해도 폰 광고가 계속 보이면 onResult가
    // 다시 큐잉하며, 쿨다운(10초)이 재시도 간격을 보장함.
    _pending_valid = false;
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
