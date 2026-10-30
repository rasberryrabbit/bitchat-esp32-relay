/*
 * wifi_bridge.cpp - WiFi + MQTT 브릿지 구현
 *
 * 한국어:
 *   WiFi STA 모드로 AP 연결 -> MQTT 브로커 연결
 *   - BLE 패킷 -> base64 인코딩 -> MQTT publish (bchat/tx topic)
 *   - MQTT subscribe (bchat/rx/{node_id}) -> base64 디코딩 -> BLE 큐로 enqueue
 *
 *   Serial 명령으로 실시간 on/off:
 *     wifi                  - 상태 출력
 *     wifi on               - WiFi+MQTT 연결
 *     wifi off              - 연결 해제
 *     wifi stat             - 통계 출력
 *     wifi set <s> <p>      - SSID/password NVS 저장 (영속화)
 *     wifi broker <host>    - MQTT broker 변경 (RAM만, 재부팅 시 기본값)
 */

#include "wifi_bridge.h"
#include "log.h"
#include "ble_relay.h"
#include "bitchat_protocol.h"
#include "storage.h"   // 자체 node_id 조회용
#include "node_config.h"   // SPIFFS-based wifi/mqtt config

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <base64.h>
#include <string.h>

// mTLS 인증서 파일 경로 (PEM 형식)
#define BROKER_CA_FILE    "/broker_ca.pem"
#define CLIENT_CERT_FILE  "/client_cert.pem"
#define CLIENT_KEY_FILE   "/client_key.pem"

// 상태
static bool g_init_done = false;
static bool g_enabled = false;          // 사용자가 wifi on 했는지
static bool g_wifi_connected = false;
static bool g_mqtt_connected = false;
static WiFiClient g_wifi_client;             // plain MQTT (1883) + socket client 공용
static WiFiClientSecure g_wifi_secure;       // mTLS MQTT (8883)
static PubSubClient g_mqtt_client(g_wifi_client);  // 초기엔 plain, init에서 secure로 교체
static char g_mqtt_broker[64] = BC_MQTT_BROKER;
static uint16_t g_mqtt_port = BC_MQTT_PORT;
static char g_topic_subscribe[40];      // bchat/rx/{node_id}
static char g_topic_publish[20];        // bchat/tx
static bool g_use_tls = false;          // mTLS 활성화 여부 (boot 시 /wifi.conf에서 로드)

// v19: transport mode (socket 직접 통신 vs MQTT)
//   기본: SOCKET (NVS에서 로드, 기본값 1=SOCKET)
//   socket 모드: TCP 클라이언트로 서버에 직접 연결, binary length-prefix 프레이밍
//   mqtt 모드: 기존 MQTT 브로커 (binary payload, QoS 1)
static bc_net_mode_t g_transport_mode = BC_NET_MODE_SOCKET;
static char g_socket_host[64] = "";     // socket 서버 주소 (NVS에서 로드)
static uint16_t g_socket_port = 8765;   // socket 서버 포트 (NVS에서 로드)
static bool g_socket_connected = false;
static WiFiClient g_socket_client;      // socket 클라이언트 (MQTT와 별도)
// socket 수신 버퍼 (length-prefix 프레이밍: 2B length + payload)
static uint8_t g_socket_rx_buf[BITCHAT_MAX_PKT_LEN];
static size_t g_socket_rx_len = 0;      // 현재 프레임의 예상 payload 길이
static bool g_socket_rx_header = false; // length 헤더 수신 여부

// v20: UDP protocol (TCP 대안)
//   프로토콜: [1B type][2B seq BE][2B payload_len BE][payload...]
//   type: 0x01=DATA, 0x02=ACK
//   UDP 유니캐스트: host:port로 전송 + ACK 재전송
//   UDP 브로드캐스트: 255.255.255.255:port로 전송 (LAN 자체 완결, host 불필요)
static bc_net_proto_t g_net_proto = BC_NET_PROTO_TCP;
static WiFiUDP g_udp;
static bool g_udp_started = false;
static IPAddress g_udp_dest;           // 유니캐스트 대상 IP
// 재전송 큐 (최대 4개 동시 미확인 패킷)
//   data 필드를 malloc으로 heap에 할당 → 전역 RAM 4×12B=48B만 사용
//   (이전: data[744] 배열 → 4×752B=3KB 전역 RAM, ESP32 classic overflow)
#define BC_UDP_MAX_PENDING 4
#define BC_UDP_RETX_MS 500             // 첫 재전송 500ms
#define BC_UDP_MAX_RETRIES 3
struct udp_pending_t {
    uint16_t seq;
    uint32_t last_send_ms;
    uint8_t retries;
    uint8_t len;                       // payload 길이
    uint8_t* data;                     // malloc된 버퍼 (len 크기, NULL=빈 슬롯)
};
static struct udp_pending_t g_udp_pending[BC_UDP_MAX_PENDING];
static uint16_t g_udp_seq_next = 1;
// 최근 수신 seq (중복 DATA → ACK만 재전송, BLE 큐 안 넣음)
#define BC_UDP_RX_HISTORY 16
static uint16_t g_udp_rx_seen[BC_UDP_RX_HISTORY];
static uint8_t g_udp_rx_seen_idx = 0;

// 통계
static uint32_t g_packets_tx = 0;       // MQTT로 publish한 패킷 수
static uint32_t g_packets_rx = 0;       // MQTT에서 받은 패킷 수
static uint32_t g_publish_fail = 0;
static uint32_t g_decode_fail = 0;

// --- base64 인코딩/디코딩 (Arduino base64 class 사용) ---
// binary data를 안전하게 MQTT payload로 전송하기 위해

static size_t base64_encode(const uint8_t* src, size_t len, char* out, size_t out_max) {
    // base64 encoded length = ceil(len/3)*4 + 1 (NUL)
    size_t enc_len = ((len + 2) / 3) * 4 + 1;
    if (enc_len > out_max) return 0;
    // Arduino base64::encode는 String 반환
    String encoded = base64::encode(src, len);
    if (encoded.length() + 1 > out_max) return 0;
    strcpy(out, encoded.c_str());
    return encoded.length();
}

static size_t base64_decode(const char* src, size_t src_len, uint8_t* out, size_t out_max) {
    // Arduino base64 클래스는 decode 미지원 -> 직접 구현
    static const int8_t b64_table[256] = {0};
    static bool table_init = false;
    static int8_t lookup[256];
    if (!table_init) {
        memset(lookup, -1, sizeof(lookup));
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) lookup[(uint8_t)chars[i]] = i;
        table_init = true;
    }
    // 패딩 '=' 제거
    size_t real_len = src_len;
    while (real_len > 0 && src[real_len-1] == '=') real_len--;

    size_t out_pos = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < real_len; ++i) {
        int8_t v = lookup[(uint8_t)src[i]];
        if (v < 0) return 0;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_pos >= out_max) return 0;
            out[out_pos++] = (uint8_t)((buf >> bits) & 0xFF);
        }
    }
    return out_pos;
}

// --- v19: forward declarations (socket/mqtt/udp 내부 함수) ---
static bool mqtt_publish_binary(const uint8_t* data, size_t len);
static bool socket_publish(const uint8_t* data, size_t len);
static bool socket_connect(void);
static void socket_disconnect(void);
static void socket_loop(void);
// v20: UDP 엔진
static bool udp_start(void);
static void udp_stop(void);
static bool udp_publish(const uint8_t* data, size_t len);
static void udp_loop(void);
static void udp_send_ack(uint16_t seq, IPAddress& remote, uint16_t remote_port);
static void udp_process_ack(uint16_t seq);
static bool udp_rx_seen(uint16_t seq);
static void udp_rx_mark(uint16_t seq);
static void udp_retx_check(void);

// --- MQTT 콜백 ---
//   이전: base64-encoded payload → decode → BLE 큐
//   v19:  binary payload 직접 → BLE 큐 (CPU/대역폭 33% 절약)
static void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    LOGD("mqtt RX topic=%s len=%u", topic, length);

    if (length == 0 || length > BITCHAT_MAX_PKT_LEN) {
        g_decode_fail++;
        LOGW("mqtt: payload out of range (%u)", length);
        return;
    }
    if (length < BITCHAT_MIN_PKT_LEN) {
        g_decode_fail++;
        LOGW("mqtt: payload too short (%u)", length);
        return;
    }

    g_packets_rx++;
    // binary payload를 BLE 큐에 직접 enqueue
    g_relay.enqueue_packet(payload, length, BC_SRC_CLIENT_NOTIFY, 0xFFFC);
    LOGD("mqtt->BLE: enqueued %u bytes", length);
}

bool wifi_bridge_init(void) {
    if (g_init_done) return true;

    // SPIFFS에서 wifi/mqtt 설정 로드 (node_config_init 선행)
    node_config_init();
    const bc_wifi_config_t* cfg = node_config_get_wifi();

    // 전역 변수 갱신 (RAM 캐시)
    strncpy(g_mqtt_broker, cfg->mqtt_broker, sizeof(g_mqtt_broker) - 1);
    g_mqtt_broker[sizeof(g_mqtt_broker) - 1] = 0;
    g_mqtt_port = cfg->mqtt_port;
    g_use_tls = cfg->use_tls;

    // 자체 node_id로 subscribe topic 설정
    extern bitchat_storage_t g_storage;
    char node_id[BITCHAT_ID_STR_LEN];
    bitchat_id_from_pk(g_storage.self_kp.public_key, node_id);
    snprintf(g_topic_subscribe, sizeof(g_topic_subscribe),
             "%s/rx/%s", cfg->mqtt_topic_prefix, node_id);
    snprintf(g_topic_publish, sizeof(g_topic_publish),
             "%s/tx", cfg->mqtt_topic_prefix);

    // TLS 모드면 인증서 로드
    if (g_use_tls) {
        // SPIFFS에서 PEM 파일 로드 (CA 필수, client cert/key는 mTLS 시)
        int cert_mask = node_config_check_certs();
        if ((cert_mask & 1) == 0) {
            LOGE("wifi_bridge: use_tls=ON but /broker_ca.pem missing");
            LOGE("  → use 'cert upload ca' to provide CA cert (PEM format)");
            LOGW("  → falling back to plain MQTT (port 1883)");
            g_use_tls = false;
            g_mqtt_port = 1883;
        } else {
            // CA 로드 (필수)
            File ca_file = SPIFFS.open(BROKER_CA_FILE, "r");
            if (ca_file) {
                size_t ca_size = ca_file.size();
                g_wifi_secure.loadCACert(ca_file, ca_size);
                ca_file.close();
                LOGI("wifi_bridge: loaded broker CA from %s (%u bytes)",
                     BROKER_CA_FILE, (unsigned)ca_size);
            }
            // client cert + key 로드 (mTLS 시)
            if (cert_mask & 2) {
                File cert_file = SPIFFS.open(CLIENT_CERT_FILE, "r");
                if (cert_file) {
                    size_t cert_size = cert_file.size();
                    g_wifi_secure.loadCertificate(cert_file, cert_size);
                    cert_file.close();
                    LOGI("wifi_bridge: loaded client cert from %s (%u bytes)",
                         CLIENT_CERT_FILE, (unsigned)cert_size);
                }
            }
            if (cert_mask & 4) {
                File key_file = SPIFFS.open(CLIENT_KEY_FILE, "r");
                if (key_file) {
                    size_t key_size = key_file.size();
                    g_wifi_secure.loadPrivateKey(key_file, key_size);
                    key_file.close();
                    LOGI("wifi_bridge: loaded client key from %s (%u bytes)",
                         CLIENT_KEY_FILE, (unsigned)key_size);
                }
            }
            // PubSubClient에 secure client 주입
            g_mqtt_client.setClient(g_wifi_secure);
            LOGI("wifi_bridge: mTLS enabled (CA + cert=%s + key=%s)",
                 (cert_mask & 2) ? "loaded" : "missing",
                 (cert_mask & 4) ? "loaded" : "missing");
        }
    } else {
        // plain MQTT - WiFiClient 사용
        g_mqtt_client.setClient(g_wifi_client);
    }

    // v19: MQTT buffer 4096으로 확장 (이전 1488) + QoS 1 지원
    //   base64 제거로 payload가 binary지만 큰 패킷(744B) + MQTT 헤더 여유
    g_mqtt_client.setServer(g_mqtt_broker, g_mqtt_port);
    g_mqtt_client.setBufferSize(4096);
    g_mqtt_client.setCallback(mqtt_callback);
    g_mqtt_client.setKeepAlive(30);

    // v19/v20: transport mode + socket 설정 + proto SPIFFS(/wifi.conf)에서 로드
    const bc_wifi_config_t* wcfg = node_config_get_wifi();
    g_transport_mode = (bc_net_mode_t)wcfg->net_mode;
    strncpy(g_socket_host, wcfg->socket_host, sizeof(g_socket_host) - 1);
    g_socket_host[sizeof(g_socket_host) - 1] = 0;
    g_socket_port = wcfg->socket_port;
    g_net_proto = (bc_net_proto_t)wcfg->net_proto;

    g_init_done = true;
    LOGI("wifi_bridge: ready (mode=%s, proto=%s, broker=%s:%u, tls=%s, socket=%s:%u)",
         wifi_bridge_mode_str(g_transport_mode),
         wifi_bridge_mode_str(g_transport_mode) == std::string("SOCKET") ?
             wifi_bridge_proto_str(g_net_proto) : "N/A",
         g_mqtt_broker, g_mqtt_port, g_use_tls ? "ON" : "OFF",
         g_socket_host[0] ? g_socket_host : "(not set)", g_socket_port);
    LOGI("wifi_bridge: mqtt subscribe=%s publish=%s",
         g_topic_subscribe, g_topic_publish);
    return true;
}

bool wifi_bridge_begin_connection(const char* ssid, const char* password) {
    if (!g_init_done) wifi_bridge_init();

    // SSID/password 결정: 인자 > SPIFFS(/wifi.conf) > 컴파일 타임 매크로
    char actual_ssid[64] = {0};
    char actual_pass[64] = {0};

    if (ssid && password) {
        strncpy(actual_ssid, ssid, sizeof(actual_ssid) - 1);
        strncpy(actual_pass, password, sizeof(actual_pass) - 1);
    } else {
        // SPIFFS에서 조회 (구버전 호환용으로 NVS도 fallback 확인)
        if (!wifi_bridge_load_credentials(actual_ssid, sizeof(actual_ssid),
                                           actual_pass, sizeof(actual_pass))) {
            // SPIFFS/NVS 모두 없으면 컴파일 타임 매크로
#ifdef BC_WIFI_SSID
            strncpy(actual_ssid, BC_WIFI_SSID, sizeof(actual_ssid) - 1);
            strncpy(actual_pass, BC_WIFI_PASS, sizeof(actual_pass) - 1);
#else
            LOGE("wifi: no SSID/password configured (use 'wifi set <ssid> <pass>' "
                 "or -DBC_WIFI_SSID=...)");
            return false;
#endif
        }
    }

    if (actual_ssid[0] == 0) {
        LOGE("wifi: empty SSID");
        return false;
    }

    LOGI("wifi: connecting to SSID=%s ...", actual_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(actual_ssid, actual_pass);

    // 최대 15초 대기
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(200);
        if ((millis() - start) % 1000 < 200) {
            Serial.print(".");
        }
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        LOGE("wifi: connection failed (status=%d)", WiFi.status());
        g_wifi_connected = false;
        return false;
    }
    g_wifi_connected = true;
    g_enabled = true;
    LOGI("wifi: connected! IP=%s, RSSI=%d dBm",
         WiFi.localIP().toString().c_str(), WiFi.RSSI());

    // v19: transport mode에 따라 socket 또는 MQTT 연결
    if (g_transport_mode == BC_NET_MODE_SOCKET) {
        // v20: proto에 따라 TCP / UDP / UDP-bcast
        if (g_net_proto == BC_NET_PROTO_TCP) {
            LOGI("net mode=SOCKET proto=TCP, connecting to %s:%u ...",
                 g_socket_host[0] ? g_socket_host : "(not set)", g_socket_port);
            socket_connect();
        } else {
            // UDP 또는 UDP-BCAST
            LOGI("net mode=SOCKET proto=%s, starting UDP on port %u ...",
                 wifi_bridge_proto_str(g_net_proto), g_socket_port);
            if (g_net_proto == BC_NET_PROTO_UDP && g_socket_host[0] == 0) {
                LOGW("udp unicast: no host set (use 'net host <ip> <port>')");
            }
            udp_start();
        }
    } else if (g_transport_mode == BC_NET_MODE_MQTT) {
        // MQTT 브로커 연결
        LOGI("mqtt: connecting to broker %s:%u ...", g_mqtt_broker, g_mqtt_port);
        char client_id[32];
        snprintf(client_id, sizeof(client_id), "bchat-%s", 
                 g_topic_subscribe + strlen(BC_MQTT_TOPIC_PREFIX) + 4);

        if (g_mqtt_client.connect(client_id)) {
            g_mqtt_connected = true;
            LOGI("mqtt: connected as %s", client_id);
            if (g_mqtt_client.subscribe(g_topic_subscribe)) {
                LOGI("mqtt: subscribed to %s", g_topic_subscribe);
            } else {
                LOGW("mqtt: subscribe failed");
            }
        } else {
            LOGW("mqtt: connect failed (state=%d)", g_mqtt_client.state());
            g_mqtt_connected = false;
        }
    } else {
        LOGI("net mode=OFF, WiFi only (no internet bridge)");
    }

    return true;
}

void wifi_bridge_disconnect(void) {
    // v19/v20: socket + UDP + MQTT 모두 해제
    socket_disconnect();
    udp_stop();
    if (g_mqtt_connected) {
        g_mqtt_client.unsubscribe(g_topic_subscribe);
        g_mqtt_client.disconnect();
        g_mqtt_connected = false;
        LOGI("mqtt: disconnected");
    }
    if (g_wifi_connected) {
        WiFi.disconnect(true);
        g_wifi_connected = false;
        LOGI("wifi: disconnected");
    }
    g_enabled = false;
}

// v19/v20: is_connected - transport mode + proto에 따라
bool wifi_bridge_is_connected(void) {
    if (!g_wifi_connected) return false;
    if (g_transport_mode == BC_NET_MODE_SOCKET) {
        if (g_net_proto == BC_NET_PROTO_TCP) return g_socket_connected;
        // UDP/UDP-BCAST: g_udp_started로 판단
        return g_udp_started;
    }
    if (g_transport_mode == BC_NET_MODE_MQTT) return g_mqtt_connected;
    return false;  // OFF
}

bool wifi_bridge_is_enabled(void) {
    return g_enabled;
}

void wifi_bridge_loop(void) {
    if (!g_init_done || !g_wifi_connected) return;
    // WiFi가 끊어졌으면 자동 재연결 시도
    if (WiFi.status() != WL_CONNECTED) {
        LOGW("wifi: connection lost, retrying...");
        WiFi.reconnect();
        delay(1000);
        if (WiFi.status() != WL_CONNECTED) {
            g_wifi_connected = false;
            return;
        }
    }
    // v19/v20: transport mode + proto에 따라 분기
    if (g_transport_mode == BC_NET_MODE_SOCKET) {
        if (g_net_proto == BC_NET_PROTO_TCP) {
            socket_loop();
        } else {
            udp_loop();
        }
    } else if (g_transport_mode == BC_NET_MODE_MQTT) {
        // MQTT 연결 끊어졌으면 재연결
        if (!g_mqtt_client.connected() && g_mqtt_connected) {
            LOGW("mqtt: disconnected, retrying...");
            if (g_mqtt_client.connect("bchat-recon")) {
                g_mqtt_client.subscribe(g_topic_subscribe);
                LOGI("mqtt: reconnected");
            } else {
                g_mqtt_connected = false;
                return;
            }
        }
        g_mqtt_client.loop();
    }
    // OFF mode: WiFi만 유지, internet bridge 없음
}

// v19/v20: wifi_bridge_publish - transport mode + proto에 따라 전송
bool wifi_bridge_publish(const uint8_t* data, size_t len) {
    if (!data || len == 0 || len > BITCHAT_MAX_PKT_LEN) return false;

    if (g_transport_mode == BC_NET_MODE_SOCKET) {
        if (g_net_proto == BC_NET_PROTO_TCP) {
            return socket_publish(data, len);
        } else {
            return udp_publish(data, len);
        }
    } else if (g_transport_mode == BC_NET_MODE_MQTT) {
        return mqtt_publish_binary(data, len);
    }
    return false;  // OFF mode
}

// v19: MQTT binary publish (base64 제거)
//   PubSubClient 2.8은 QoS 인자 미지원 → QoS 0 (fire-and-forget)
//   base64 제거로 대역폭 33% 절약 효과는 유지
static bool mqtt_publish_binary(const uint8_t* data, size_t len) {
    if (!g_mqtt_connected) return false;
    // binary payload 직접 publish (base64 인코딩 제거)
    if (!g_mqtt_client.publish(g_topic_publish, data, len, false)) {
        g_publish_fail++;
        LOGW("wifi: mqtt publish fail (total_fail=%u)", g_publish_fail);
        return false;
    }
    g_packets_tx++;
    LOGD("wifi: published %u bytes (binary) to %s, total=%u",
         (unsigned)len, g_topic_publish, g_packets_tx);
    return true;
}

// v19: socket binary publish (length-prefix 프레이밍: 2B BE length + payload)
static bool socket_publish(const uint8_t* data, size_t len) {
    if (!g_socket_connected) return false;
    // 2바이트 big-endian length 헤더
    uint8_t hdr[2];
    hdr[0] = (uint8_t)((len >> 8) & 0xFF);
    hdr[1] = (uint8_t)(len & 0xFF);
    if (g_socket_client.write(hdr, 2) != 2) {
        g_publish_fail++;
        LOGW("socket: write header fail");
        return false;
    }
    if (g_socket_client.write(data, len) != (int)len) {
        g_publish_fail++;
        LOGW("socket: write payload fail");
        return false;
    }
    g_packets_tx++;
    LOGD("socket: sent %u bytes to %s:%u, total=%u",
         (unsigned)len, g_socket_host, g_socket_port, g_packets_tx);
    return true;
}

// v19: socket 연결 (TCP 클라이언트)
static bool socket_connect(void) {
    if (g_socket_host[0] == 0) {
        LOGW("socket: no host set (use 'net host <ip> <port>')");
        return false;
    }
    LOGI("socket: connecting to %s:%u ...", g_socket_host, g_socket_port);
    if (!g_socket_client.connect(g_socket_host, g_socket_port)) {
        LOGW("socket: connect failed to %s:%u", g_socket_host, g_socket_port);
        g_socket_connected = false;
        return false;
    }
    g_socket_connected = true;
    g_socket_rx_header = false;
    g_socket_rx_len = 0;
    LOGI("socket: connected to %s:%u", g_socket_host, g_socket_port);
    return true;
}

// v19: socket 연결 해제
static void socket_disconnect(void) {
    if (g_socket_connected) {
        g_socket_client.stop();
        g_socket_connected = false;
        LOGI("socket: disconnected");
    }
    g_socket_rx_header = false;
    g_socket_rx_len = 0;
}

// v19: socket 루프 - length-prefix 프레이밍 수신 처리
//   프레임 형식: [2B big-endian length][payload]
//   length 읽기 → payload 읽기 → BLE 큐 enqueue
static void socket_loop(void) {
    if (!g_socket_connected) {
        // 자동 재연결 시도
        if (g_socket_host[0] && g_wifi_connected) {
            static uint32_t last_retry = 0;
            if (millis() - last_retry > 5000) {
                last_retry = millis();
                socket_connect();
            }
        }
        return;
    }
    // 연결 끊김 체크
    if (!g_socket_client.connected()) {
        g_socket_connected = false;
        LOGW("socket: connection lost");
        return;
    }
    // 수신 데이터 처리
    while (g_socket_client.available()) {
        if (!g_socket_rx_header) {
            // length 헤더 (2바이트) 수신
            if (g_socket_client.available() < 2) break;
            uint8_t hdr[2];
            g_socket_client.read(hdr, 2);
            g_socket_rx_len = ((size_t)hdr[0] << 8) | hdr[1];
            if (g_socket_rx_len == 0 || g_socket_rx_len > BITCHAT_MAX_PKT_LEN) {
                LOGW("socket: invalid frame length %u, resetting", (unsigned)g_socket_rx_len);
                g_socket_rx_len = 0;
                continue;
            }
            g_socket_rx_header = true;
        }
        if (g_socket_rx_header) {
            // payload 수신
            size_t avail = g_socket_client.available();
            // 전체 payload가 올 때까지 대기 (논블로킹: 다음 루프에서 재시도)
            if (avail < g_socket_rx_len) break;
            int read = g_socket_client.read(g_socket_rx_buf, g_socket_rx_len);
            if (read == (int)g_socket_rx_len) {
                g_packets_rx++;
                g_relay.enqueue_packet(g_socket_rx_buf, g_socket_rx_len,
                                       BC_SRC_CLIENT_NOTIFY, 0xFFFC);
                LOGD("socket->BLE: enqueued %u bytes", (unsigned)g_socket_rx_len);
            } else {
                LOGW("socket: short read (%d/%u)", read, (unsigned)g_socket_rx_len);
            }
            g_socket_rx_header = false;
            g_socket_rx_len = 0;
        }
    }
}

bool wifi_bridge_save_credentials(const char* ssid, const char* password) {
    // SPIFFS(/wifi.conf)에 저장 - node_config 모듈 사용
    bool ok = node_config_set_wifi_credentials(ssid, password);
    LOGI("wifi: credentials saved to SPIFFS /wifi.conf (ssid=%s, ok=%d)",
         ssid ? ssid : "(null)", ok);
    return ok;
}

bool wifi_bridge_load_credentials(char* out_ssid, size_t ssid_max,
                                   char* out_pass, size_t pass_max) {
    if (!out_ssid || !out_pass || ssid_max == 0 || pass_max == 0) return false;
    out_ssid[0] = 0;
    out_pass[0] = 0;

    // 1차: SPIFFS /wifi.conf에서 조회
    const bc_wifi_config_t* cfg = node_config_get_wifi();
    if (cfg->ssid[0]) {
        strncpy(out_ssid, cfg->ssid, ssid_max - 1);
        out_ssid[ssid_max - 1] = 0;
        strncpy(out_pass, cfg->pass, pass_max - 1);
        out_pass[pass_max - 1] = 0;
        return true;
    }

    // 2차: 구버전 호환 - NVS에서 조회 (1회 마이그레이션 안 된 경우)
    nvs_handle_t h;
    if (nvs_open(BC_WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = true;
    if (nvs_get_str(h, BC_WIFI_NVS_SSID, out_ssid, &ssid_max) != ESP_OK) ok = false;
    if (nvs_get_str(h, BC_WIFI_NVS_PASS, out_pass, &pass_max) != ESP_OK) ok = false;
    nvs_close(h);
    if (!ok) {
        out_ssid[0] = 0;
        out_pass[0] = 0;
    }
    return ok;
}

// ----- MQTT broker/port/prefix 설정 (SPIFFS 영속화) -----

bool wifi_bridge_set_broker(const char* broker, uint16_t port) {
    if (!broker) return false;
    if (port == 0) port = 1883;  // 기본값
    bool ok = node_config_set_mqtt_broker(broker, port);
    LOGI("wifi: mqtt broker → %s:%u (SPIFFS, ok=%d)", broker, port, ok);
    LOGW("wifi: run 'wifi off' then 'wifi on' to apply changes");
    return ok;
}

bool wifi_bridge_set_topic_prefix(const char* prefix) {
    if (!prefix) return false;
    bool ok = node_config_set_mqtt_topic_prefix(prefix);
    LOGI("wifi: mqtt topic prefix → %s (SPIFFS, ok=%d)", prefix, ok);
    LOGW("wifi: run 'wifi off' then 'wifi on' to apply changes");
    return ok;
}

// ----- mTLS 설정 -----

bool wifi_bridge_set_tls_enabled(bool on) {
    if (on) {
        // 인증서 존재 확인 (CA는 필수)
        int mask = node_config_check_certs();
        if ((mask & 1) == 0) {
            LOGE("wifi: cannot enable TLS - /broker_ca.pem missing");
            LOGE("  → use 'cert upload ca' to provide CA cert (PEM format)");
            return false;
        }
        if (mask != 7) {
            LOGW("wifi: TLS enabled but some certs missing (mask=0x%x)", mask);
            if ((mask & 2) == 0) LOGW("  → /client_cert.pem missing (mTLS requires)");
            if ((mask & 4) == 0) LOGW("  → /client_key.pem missing (mTLS requires)");
            LOGW("  → TLS will work as CA-only mode (server verified, client not)");
        }
    }
    bool ok = node_config_set_tls_enabled(on);
    LOGI("wifi: tls → %s (SPIFFS, ok=%d, port=%u)",
         on ? "ON" : "OFF", ok, on ? 8883 : 1883);
    LOGW("wifi: run 'wifi off' then 'wifi on' to apply changes");
    return ok;
}

bool wifi_bridge_get_tls_enabled(void) {
    if (!g_init_done) wifi_bridge_init();
    return g_use_tls;
}

int wifi_bridge_check_certs(void) {
    return node_config_check_certs();
}

const char* wifi_bridge_get_broker(void) {
    if (!g_init_done) wifi_bridge_init();
    return g_mqtt_broker;
}

uint16_t wifi_bridge_get_port(void) {
    if (!g_init_done) wifi_bridge_init();
    return g_mqtt_port;
}

const char* wifi_bridge_get_topic_prefix(void) {
    if (!g_init_done) wifi_bridge_init();
    // /wifi.conf의 topic_prefix를 node_config에서 직접 가져옴
    return node_config_get_wifi()->mqtt_topic_prefix;
}

void wifi_bridge_debug_dump(void) {
    LOGI("=== wifi_bridge dump ===");
    LOGI("mode: %s  enabled: %d  wifi: %d  socket: %d  mqtt: %d",
         wifi_bridge_mode_str(g_transport_mode),
         g_enabled, g_wifi_connected, g_socket_connected, g_mqtt_connected);
    if (g_wifi_connected) {
        LOGI("ip: %s  rssi: %d dBm",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    if (g_transport_mode == BC_NET_MODE_SOCKET && g_socket_host[0]) {
        LOGI("socket: %s:%u %s", g_socket_host, g_socket_port,
             g_socket_connected ? "(connected)" : "(disconnected)");
    }
    if (g_transport_mode == BC_NET_MODE_MQTT && g_mqtt_connected) {
        LOGI("broker: %s:%u", g_mqtt_broker, g_mqtt_port);
        LOGI("subscribed: %s", g_topic_subscribe);
        LOGI("publish:    %s", g_topic_publish);
    }
    LOGI("stats: tx=%u rx=%u pub_fail=%u decode_fail=%u",
         g_packets_tx, g_packets_rx, g_publish_fail, g_decode_fail);
}

// ===== v19: transport mode + socket 설정 (공개 API) =====

const char* wifi_bridge_mode_str(bc_net_mode_t mode) {
    switch (mode) {
        case BC_NET_MODE_OFF:    return "OFF";
        case BC_NET_MODE_SOCKET: return "SOCKET";
        case BC_NET_MODE_MQTT:   return "MQTT";
        default:                 return "UNKNOWN";
    }
}

bc_net_mode_t wifi_bridge_get_mode(void) {
    return g_transport_mode;
}

bool wifi_bridge_set_mode(bc_net_mode_t mode) {
    if (mode > BC_NET_MODE_MQTT) {
        LOGE("net: invalid mode %u", mode);
        return false;
    }
    // 이미 연결 중이면 모드 전환 전 해제
    if (g_enabled) {
        LOGI("net: switching mode %s -> %s, reconnecting...",
             wifi_bridge_mode_str(g_transport_mode), wifi_bridge_mode_str(mode));
        socket_disconnect();
        if (g_mqtt_connected) {
            g_mqtt_client.disconnect();
            g_mqtt_connected = false;
        }
    }
    g_transport_mode = mode;
    // v19: SPIFFS(/wifi.conf)에 영속화 (이전: NVS)
    node_config_set_net_mode((uint8_t)mode);
    LOGI("net: mode set to %s (persisted to /wifi.conf)", wifi_bridge_mode_str(mode));
    // 재연결 시도 (WiFi가 이미 연결되어 있으면)
    if (g_enabled && g_wifi_connected) {
        if (mode == BC_NET_MODE_SOCKET) {
            socket_connect();
        } else if (mode == BC_NET_MODE_MQTT) {
            char client_id[32];
            snprintf(client_id, sizeof(client_id), "bchat-%s",
                     g_topic_subscribe + strlen(BC_MQTT_TOPIC_PREFIX) + 4);
            if (g_mqtt_client.connect(client_id)) {
                g_mqtt_connected = true;
                g_mqtt_client.subscribe(g_topic_subscribe);
                LOGI("mqtt: connected as %s", client_id);
            }
        }
    }
    return true;
}

bool wifi_bridge_set_socket_host(const char* host, uint16_t port) {
    if (!host || host[0] == 0) {
        LOGE("net: host required");
        return false;
    }
    if (port == 0) port = 8765;
    strncpy(g_socket_host, host, sizeof(g_socket_host) - 1);
    g_socket_host[sizeof(g_socket_host) - 1] = 0;
    g_socket_port = port;
    // v19: SPIFFS(/wifi.conf)에 영속화 (이전: NVS)
    node_config_set_socket_host(g_socket_host);
    node_config_set_socket_port(port);
    LOGI("net: socket host set to %s:%u (persisted to /wifi.conf)",
         g_socket_host, g_socket_port);
    // 이미 socket 모드이고 WiFi 연결되어 있으면 재연결
    if (g_transport_mode == BC_NET_MODE_SOCKET && g_wifi_connected) {
        if (g_net_proto == BC_NET_PROTO_TCP) {
            socket_disconnect();
            socket_connect();
        } else {
            // UDP 모드: dest IP 갱신을 위해 restart
            udp_stop();
            udp_start();
        }
    }
    return true;
}

const char* wifi_bridge_get_socket_host(void) {
    return g_socket_host;
}

uint16_t wifi_bridge_get_socket_port(void) {
    return g_socket_port;
}

// ===== v20: UDP + ACK 엔진 =====
//
// 프로토콜 프레임:
//   DATA: [1B 0x01][2B seq BE][2B payload_len BE][payload...]
//   ACK:  [1B 0x02][2B seq BE]
//
// 재전송: 500ms 첫 retx, 최대 3회 (500/1000/2000ms backoff)
// 중복 수신: ACK만 재전송, BLE 큐에 안 넣음 (dedup)
// 브로드캐스트 모드: 255.255.255.255:port, host 불필요

// UDP 프레임 헤더 빌드
static void udp_build_frame(uint8_t type, uint16_t seq, uint16_t payload_len,
                             uint8_t* out_hdr) {
    out_hdr[0] = type;
    out_hdr[1] = (uint8_t)(seq >> 8);
    out_hdr[2] = (uint8_t)(seq & 0xFF);
    out_hdr[3] = (uint8_t)(payload_len >> 8);
    out_hdr[4] = (uint8_t)(payload_len & 0xFF);
}

static bool udp_start(void) {
    if (g_udp_started) return true;
    if (!g_udp.begin(g_socket_port)) {
        LOGE("udp: begin(port=%u) failed", g_socket_port);
        return false;
    }
    g_udp_started = true;
    // 유니캐스트 대상 IP 파싱
    if (g_socket_host[0] && g_net_proto == BC_NET_PROTO_UDP) {
        g_udp_dest.fromString(g_socket_host);
    }
    // 브로드캐스트 모드: broadcast 허용
    if (g_net_proto == BC_NET_PROTO_UDP_BCAST) {
        g_udp_dest = IPAddress(255, 255, 255, 255);
    }
    LOGI("udp: started (port=%u, proto=%s, dest=%s)",
         g_socket_port,
         g_net_proto == BC_NET_PROTO_UDP_BCAST ? "BCAST" : "UNICAST",
         g_net_proto == BC_NET_PROTO_UDP_BCAST ? "255.255.255.255" : g_socket_host);
    return true;
}

static void udp_stop(void) {
    if (g_udp_started) {
        g_udp.stop();
        g_udp_started = false;
        LOGI("udp: stopped");
    }
    // 재전송 큐 초기화 (malloc된 버퍼들 해제)
    for (int i = 0; i < BC_UDP_MAX_PENDING; i++) {
        if (g_udp_pending[i].data) {
            free(g_udp_pending[i].data);
            g_udp_pending[i].data = nullptr;
        }
    }
}

// UDP DATA 전송 + 재전송 큐에 등록
static bool udp_publish(const uint8_t* data, size_t len) {
    if (!g_udp_started) {
        if (!udp_start()) return false;
    }
    if (!data || len == 0 || len > BITCHAT_MAX_PKT_LEN) return false;

    // 빈 재전송 슬롯 찾기
    int slot = -1;
    for (int i = 0; i < BC_UDP_MAX_PENDING; i++) {
        if (g_udp_pending[i].data == nullptr) { slot = i; break; }
    }
    if (slot < 0) {
        LOGW("udp: pending queue full, dropping (max=%d)", BC_UDP_MAX_PENDING);
        return false;
    }

    uint16_t seq = g_udp_seq_next++;
    if (g_udp_seq_next == 0) g_udp_seq_next = 1;  // 0 피함

    // 프레임 빌드: 헤더(5) + payload
    uint8_t hdr[5];
    udp_build_frame(0x01, seq, (uint16_t)len, hdr);

    // 전송
    g_udp.beginPacket(g_udp_dest, g_socket_port);
    g_udp.write(hdr, 5);
    g_udp.write(data, len);
    if (!g_udp.endPacket()) {
        LOGW("udp: send fail (seq=%u)", seq);
        return false;
    }

    // 재전송 큐에 등록
    // malloc으로 버퍼 할당 (heap 사용, 전역 RAM 절약)
    uint8_t* buf = (uint8_t*)malloc(len);
    if (!buf) {
        LOGW("udp: malloc fail (%uB), dropping", (unsigned)len);
        return false;
    }
    memcpy(buf, data, len);

    g_udp_pending[slot].seq = seq;
    g_udp_pending[slot].last_send_ms = millis();
    g_udp_pending[slot].retries = 0;
    g_udp_pending[slot].len = (uint8_t)len;
    g_udp_pending[slot].data = buf;

    g_packets_tx++;
    LOGD("udp: sent seq=%u len=%u to %s:%u", seq, (unsigned)len,
         g_net_proto == BC_NET_PROTO_UDP_BCAST ? "255.255.255.255" : g_socket_host,
         g_socket_port);
    return true;
}

// ACK 전송
static void udp_send_ack(uint16_t seq, IPAddress& remote, uint16_t remote_port) {
    uint8_t ack[3];
    ack[0] = 0x02;
    ack[1] = (uint8_t)(seq >> 8);
    ack[2] = (uint8_t)(seq & 0xFF);
    g_udp.beginPacket(remote, remote_port);
    g_udp.write(ack, 3);
    g_udp.endPacket();
    LOGD("udp: sent ACK seq=%u to %s:%u", seq,
         remote.toString().c_str(), remote_port);
}

// ACK 수신 시 재전송 큐에서 제거
static void udp_process_ack(uint16_t seq) {
    for (int i = 0; i < BC_UDP_MAX_PENDING; i++) {
        if (g_udp_pending[i].data != nullptr && g_udp_pending[i].seq == seq) {
            free(g_udp_pending[i].data);
            g_udp_pending[i].data = nullptr;  // 슬롯 해제
            LOGD("udp: ACK seq=%u, cleared slot %d", seq, i);
            return;
        }
    }
    // 이미 cleared 또는 unknown seq → 무시
}

// 중복 수신 체크
static bool udp_rx_seen(uint16_t seq) {
    for (int i = 0; i < BC_UDP_RX_HISTORY; i++) {
        if (g_udp_rx_seen[i] == seq) return true;
    }
    return false;
}

static void udp_rx_mark(uint16_t seq) {
    g_udp_rx_seen[g_udp_rx_seen_idx] = seq;
    g_udp_rx_seen_idx = (g_udp_rx_seen_idx + 1) % BC_UDP_RX_HISTORY;
}

// 재전송 체크 (loop에서 호출)
static void udp_retx_check(void) {
    uint32_t now = millis();
    for (int i = 0; i < BC_UDP_MAX_PENDING; i++) {
        struct udp_pending_t* p = &g_udp_pending[i];
        if (p->data == nullptr) continue;
        uint32_t elapsed = now - p->last_send_ms;
        uint32_t threshold = BC_UDP_RETX_MS << p->retries;  // 500, 1000, 2000
        if (elapsed < threshold) continue;
        if (p->retries >= BC_UDP_MAX_RETRIES) {
            LOGW("udp: seq=%u max retries reached, dropping", p->seq);
            free(p->data);
            p->data = nullptr;  // drop
            g_publish_fail++;
            continue;
        }
        // 재전송
        p->retries++;
        uint8_t hdr[5];
        udp_build_frame(0x01, p->seq, p->len, hdr);
        g_udp.beginPacket(g_udp_dest, g_socket_port);
        g_udp.write(hdr, 5);
        g_udp.write(p->data, p->len);
        g_udp.endPacket();
        p->last_send_ms = now;
        LOGD("udp: retx seq=%u attempt=%u", p->seq, p->retries);
    }
}

// UDP 수신 처리 루프
static void udp_loop(void) {
    if (!g_udp_started) {
        if (g_wifi_connected) {
            static uint32_t last_retry = 0;
            if (millis() - last_retry > 5000) {
                last_retry = millis();
                udp_start();
            }
        }
        return;
    }
    // 재전송 체크
    udp_retx_check();
    // 수신 패킷 처리
    int pkt_size = g_udp.parsePacket();
    while (pkt_size > 0) {
        // 최소 헤더: type(1) + seq(2) = 3바이트 (ACK)
        if (pkt_size < 3) {
            g_udp.flush();
            pkt_size = g_udp.parsePacket();
            continue;
        }
        uint8_t type = g_udp.read();
        uint8_t seq_hi = g_udp.read();
        uint8_t seq_lo = g_udp.read();
        uint16_t seq = ((uint16_t)seq_hi << 8) | seq_lo;
        IPAddress remote = g_udp.remoteIP();
        uint16_t remote_port = g_udp.remotePort();

        if (type == 0x02) {
            // ACK
            udp_process_ack(seq);
        } else if (type == 0x01) {
            // DATA: payload_len(2) + payload
            if (pkt_size < 5) {
                g_udp.flush();
                pkt_size = g_udp.parsePacket();
                continue;
            }
            uint8_t len_hi = g_udp.read();
            uint8_t len_lo = g_udp.read();
            uint16_t payload_len = ((uint16_t)len_hi << 8) | len_lo;
            if (payload_len > BITCHAT_MAX_PKT_LEN || payload_len > (uint16_t)(pkt_size - 5)) {
                LOGW("udp: invalid payload_len %u (pkt=%d)", payload_len, pkt_size);
                g_udp.flush();
                pkt_size = g_udp.parsePacket();
                continue;
            }
            // payload 읽기 (g_socket_rx_buf 재사용 - RAM 절약)
            g_udp.read(g_socket_rx_buf, payload_len);
            // ACK 전송 (송신자에게)
            udp_send_ack(seq, remote, remote_port);
            // 중복 체크
            if (udp_rx_seen(seq)) {
                LOGD("udp: dup seq=%u, ACK resent only", seq);
            } else {
                udp_rx_mark(seq);
                g_packets_rx++;
                g_relay.enqueue_packet(g_socket_rx_buf, payload_len, BC_SRC_CLIENT_NOTIFY, 0xFFFC);
                LOGD("udp->BLE: enqueued %u bytes (seq=%u)", payload_len, seq);
            }
        }
        pkt_size = g_udp.parsePacket();
    }
}

// ===== v20: net protocol 공개 API =====

const char* wifi_bridge_proto_str(bc_net_proto_t proto) {
    switch (proto) {
        case BC_NET_PROTO_TCP:       return "TCP";
        case BC_NET_PROTO_UDP:       return "UDP";
        case BC_NET_PROTO_UDP_BCAST: return "UDP-BCAST";
        default:                     return "UNKNOWN";
    }
}

bc_net_proto_t wifi_bridge_get_proto(void) {
    return g_net_proto;
}

bool wifi_bridge_set_proto(bc_net_proto_t proto) {
    if (proto > BC_NET_PROTO_UDP_BCAST) {
        LOGE("net: invalid proto %u", proto);
        return false;
    }
    // 이미 실행 중이면 중지 후 재시작
    if (g_enabled && g_wifi_connected &&
        g_transport_mode == BC_NET_MODE_SOCKET) {
        LOGI("net: switching proto %s -> %s",
             wifi_bridge_proto_str(g_net_proto), wifi_bridge_proto_str(proto));
        socket_disconnect();
        udp_stop();
        g_net_proto = proto;
        node_config_set_net_proto((uint8_t)proto);
        // 재시작
        if (proto == BC_NET_PROTO_TCP) {
            socket_connect();
        } else {
            udp_start();
        }
    } else {
        g_net_proto = proto;
        node_config_set_net_proto((uint8_t)proto);
    }
    LOGI("net: proto set to %s (persisted to /wifi.conf)",
         wifi_bridge_proto_str(proto));
    return true;
}
