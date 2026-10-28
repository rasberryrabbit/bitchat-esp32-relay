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

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <base64.h>
#include <string.h>

// 상태
static bool g_init_done = false;
static bool g_enabled = false;          // 사용자가 wifi on 했는지
static bool g_wifi_connected = false;
static bool g_mqtt_connected = false;
static WiFiClient g_wifi_client;
static PubSubClient g_mqtt_client(g_wifi_client);
static char g_mqtt_broker[64] = BC_MQTT_BROKER;
static uint16_t g_mqtt_port = BC_MQTT_PORT;
static char g_topic_subscribe[40];      // bchat/rx/{node_id}
static char g_topic_publish[20];        // bchat/tx

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

// --- MQTT 콜백 ---
static void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    LOGD("mqtt RX topic=%s len=%u", topic, length);

    // payload는 base64-encoded bitchat packet
    // 디코딩해서 BLE 큐에 enqueue
    static uint8_t decoded[BITCHAT_MAX_PKT_LEN];
    if (length == 0) return;

    // null-terminate payload
    char* b64 = (char*)malloc(length + 1);
    if (!b64) return;
    memcpy(b64, payload, length);
    b64[length] = 0;

    size_t decoded_len = base64_decode(b64, length, decoded, sizeof(decoded));
    free(b64);

    if (decoded_len == 0 || decoded_len < BITCHAT_MIN_PKT_LEN) {
        g_decode_fail++;
        LOGW("mqtt: base64 decode fail or too short (%u)", (unsigned)decoded_len);
        return;
    }

    g_packets_rx++;
    // BLE 큐에 enqueue (LoRa와 동일한 출처 marker 0xFFFC 사용)
    g_relay.enqueue_packet(decoded, decoded_len, BC_SRC_CLIENT_NOTIFY, 0xFFFC);
    LOGD("mqtt->BLE: enqueued %u bytes", (unsigned)decoded_len);
}

bool wifi_bridge_init(void) {
    if (g_init_done) return true;

    // 자체 node_id로 subscribe topic 설정
    // g_storage는 main.cpp에 정의된 bitchat_storage_t 인스턴스
    extern bitchat_storage_t g_storage;
    char node_id[BITCHAT_ID_STR_LEN];
    bitchat_id_from_pk(g_storage.self_kp.public_key, node_id);
    snprintf(g_topic_subscribe, sizeof(g_topic_subscribe),
             "%s/rx/%s", BC_MQTT_TOPIC_PREFIX, node_id);
    snprintf(g_topic_publish, sizeof(g_topic_publish),
             "%s/tx", BC_MQTT_TOPIC_PREFIX);

    // MQTT 콜백 설정
    g_mqtt_client.setServer(g_mqtt_broker, g_mqtt_port);
    g_mqtt_client.setBufferSize(BITCHAT_MAX_PKT_LEN * 2);   // base64는 4/3 확장
    g_mqtt_client.setCallback(mqtt_callback);
    g_mqtt_client.setKeepAlive(30);   // 30초 keep-alive

    g_init_done = true;
    LOGI("wifi_bridge: ready (broker=%s:%u, topic_prefix=%s)",
         g_mqtt_broker, g_mqtt_port, BC_MQTT_TOPIC_PREFIX);
    LOGI("wifi_bridge: subscribe=%s publish=%s",
         g_topic_subscribe, g_topic_publish);
    return true;
}

bool wifi_bridge_begin_connection(const char* ssid, const char* password) {
    if (!g_init_done) wifi_bridge_init();

    // SSID/password 결정: 인자 > NVS > 컴파일 타임 매크로
    char actual_ssid[64] = {0};
    char actual_pass[64] = {0};

    if (ssid && password) {
        strncpy(actual_ssid, ssid, sizeof(actual_ssid) - 1);
        strncpy(actual_pass, password, sizeof(actual_pass) - 1);
    } else {
        // NVS에서 조회
        if (!wifi_bridge_load_credentials(actual_ssid, sizeof(actual_ssid),
                                           actual_pass, sizeof(actual_pass))) {
            // NVS 없으면 컴파일 타임 매크로
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

    // MQTT 브로커 연결
    LOGI("mqtt: connecting to broker %s:%u ...", g_mqtt_broker, g_mqtt_port);
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "bchat-%s", 
             g_topic_subscribe + strlen(BC_MQTT_TOPIC_PREFIX) + 4);   // node_id만 추출

    if (g_mqtt_client.connect(client_id)) {
        g_mqtt_connected = true;
        LOGI("mqtt: connected as %s", client_id);
        // subscribe
        if (g_mqtt_client.subscribe(g_topic_subscribe)) {
            LOGI("mqtt: subscribed to %s", g_topic_subscribe);
        } else {
            LOGW("mqtt: subscribe failed");
        }
    } else {
        LOGW("mqtt: connect failed (state=%d)", g_mqtt_client.state());
        g_mqtt_connected = false;
    }

    return true;
}

void wifi_bridge_disconnect(void) {
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

bool wifi_bridge_is_connected(void) {
    return g_wifi_connected && g_mqtt_connected;
}

bool wifi_bridge_is_enabled(void) {
    return g_enabled;
}

void wifi_bridge_loop(void) {
    if (!g_init_done || !g_wifi_connected) return;
    // MQTT keep-alive + 수신 패킷 처리
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

bool wifi_bridge_publish(const uint8_t* data, size_t len) {
    if (!g_mqtt_connected) return false;
    if (!data || len == 0 || len > BITCHAT_MAX_PKT_LEN) return false;

    // base64 인코딩
    char b64[BITCHAT_MAX_PKT_LEN * 2 + 4];
    size_t b64_len = base64_encode(data, len, b64, sizeof(b64) - 1);
    if (b64_len == 0) {
        LOGW("wifi: base64 encode fail");
        return false;
    }
    b64[b64_len] = 0;

    if (!g_mqtt_client.publish(g_topic_publish, b64, false)) {
        g_publish_fail++;
        LOGW("wifi: mqtt publish fail (total_fail=%u)", g_publish_fail);
        return false;
    }
    g_packets_tx++;
    LOGD("wifi: published %u bytes (b64=%u) to %s, total=%u",
         (unsigned)len, (unsigned)b64_len, g_topic_publish, g_packets_tx);
    return true;
}

bool wifi_bridge_save_credentials(const char* ssid, const char* password) {
    nvs_handle_t h;
    if (nvs_open(BC_WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, BC_WIFI_NVS_SSID, ssid);
    nvs_set_str(h, BC_WIFI_NVS_PASS, password);
    nvs_commit(h);
    nvs_close(h);
    LOGI("wifi: credentials saved to NVS (ssid=%s)", ssid);
    return true;
}

bool wifi_bridge_load_credentials(char* out_ssid, size_t ssid_max,
                                   char* out_pass, size_t pass_max) {
    nvs_handle_t h;
    if (nvs_open(BC_WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = true;
    if (nvs_get_str(h, BC_WIFI_NVS_SSID, out_ssid, &ssid_max) != ESP_OK) ok = false;
    if (nvs_get_str(h, BC_WIFI_NVS_PASS, out_pass, &pass_max) != ESP_OK) ok = false;
    nvs_close(h);
    return ok;
}

void wifi_bridge_debug_dump(void) {
    LOGI("=== wifi_bridge dump ===");
    LOGI("enabled: %d  wifi: %d  mqtt: %d",
         g_enabled, g_wifi_connected, g_mqtt_connected);
    if (g_wifi_connected) {
        LOGI("ip: %s  rssi: %d dBm",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    if (g_mqtt_connected) {
        LOGI("broker: %s:%u", g_mqtt_broker, g_mqtt_port);
        LOGI("subscribed: %s", g_topic_subscribe);
        LOGI("publish:    %s", g_topic_publish);
    }
    LOGI("stats: tx=%u rx=%u pub_fail=%u decode_fail=%u",
         g_packets_tx, g_packets_rx, g_publish_fail, g_decode_fail);
}
