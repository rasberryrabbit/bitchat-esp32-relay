/*
 * node_config.cpp - SPIFFS config file implementation
 *
 * 한국어:
 *   key=value 포맷으로 /node.conf, /wifi.conf 파일 read/write.
 *   부팅 시 1회 load → RAM 캐시. 이후 조회는 캐시에서 빠르게.
 *   설정 변경(set_nickname, set_wifi_*) 시에만 SPIFFS write 발생.
 *
 *   파일 포맷:
 *     - 줄 단위 key=value, 첫 '=' 이후 모든 문자열이 value (공백 포함 가능)
 *     - '#' 또는 ';' 시작은 주석
 *     - 빈 줄 무시
 *     - trailing whitespace 제거 (key/value 모두)
 *     - value 길이 한계 초과 시 truncate
 */

#include "node_config.h"
#include "log.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// 캐시
static node_profile_t g_profile_cache;
static bc_wifi_config_t  g_wifi_cache;
static bool g_profile_loaded = false;
static bool g_wifi_loaded = false;
static bool g_init_done = false;

// 기본값 (컴파일 타임 매크로 fallback)
#ifndef BC_MQTT_BROKER
#define BC_MQTT_BROKER  "test.mosquitto.org"
#endif
#ifndef BC_MQTT_PORT
#define BC_MQTT_PORT    1883
#endif
#ifndef BC_MQTT_TOPIC_PREFIX
#define BC_MQTT_TOPIC_PREFIX  "bchat"
#endif

// ----- helpers -----

// 줄 끝의 whitespace와 \r\n 제거
static void rtrim(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' ||
                     s[n-1] == ' ' || s[n-1] == '\t')) {
        s[--n] = 0;
    }
}

// 줄 시작의 whitespace skip
static char* ltrim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

// 파일에서 모든 key=value를 콜백으로 전달
// callback은 key, value 포인터를 받음 (줄 단위, 파일 읽기 후 자동 free)
static bool parse_config_file(const char* path,
                              void (*cb)(const char* key, const char* value, void* ctx),
                              void* ctx) {
    if (!SPIFFS.exists(path)) return false;
    File f = SPIFFS.open(path, "r");
    if (!f) {
        LOGE("node_config: open(%s) failed", path);
        return false;
    }

    char line[160];
    int line_num = 0;
    while (f.available()) {
        // readBytesUntil: delimiter까지 읽고 NUL 종결
        size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = 0;
        line_num++;

        rtrim(line);
        char* p = ltrim(line);
        if (*p == 0 || *p == '#' || *p == ';') continue;  // 빈 줄 또는 주석

        // '=' 찾기
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = p;
        char* value = ltrim(eq + 1);
        rtrim(key);

        if (key[0] == 0) continue;  // 키 비어 있으면 skip

        cb(key, value, ctx);
    }
    f.close();
    return true;
}

// /node.conf 파싱 콜백
static void profile_cb(const char* key, const char* value, void* ctx) {
    node_profile_t* p = (node_profile_t*)ctx;
    if (strcmp(key, "nickname") == 0 || strcmp(key, "nick") == 0) {
        strncpy(p->nickname, value, MAX_NICKNAME_LEN);
        p->nickname[MAX_NICKNAME_LEN] = 0;
    }
    // 향후 확장: avatar, bio 등 추가 가능
}

// /wifi.conf 파싱 콜백
static void wifi_cb(const char* key, const char* value, void* ctx) {
    bc_wifi_config_t* c = (bc_wifi_config_t*)ctx;
    if (strcmp(key, "ssid") == 0) {
        strncpy(c->ssid, value, MAX_WIFI_SSID_LEN);
        c->ssid[MAX_WIFI_SSID_LEN] = 0;
    } else if (strcmp(key, "pass") == 0 || strcmp(key, "password") == 0) {
        strncpy(c->pass, value, MAX_WIFI_PASS_LEN);
        c->pass[MAX_WIFI_PASS_LEN] = 0;
    } else if (strcmp(key, "broker") == 0 || strcmp(key, "mqtt_broker") == 0) {
        strncpy(c->mqtt_broker, value, MAX_MQTT_BROKER_LEN);
        c->mqtt_broker[MAX_MQTT_BROKER_LEN] = 0;
    } else if (strcmp(key, "port") == 0 || strcmp(key, "mqtt_port") == 0) {
        c->mqtt_port = (uint16_t)atoi(value);
    } else if (strcmp(key, "topic_prefix") == 0 ||
               strcmp(key, "mqtt_topic_prefix") == 0) {
        strncpy(c->mqtt_topic_prefix, value, MAX_TOPIC_PREFIX_LEN);
        c->mqtt_topic_prefix[MAX_TOPIC_PREFIX_LEN] = 0;
    } else if (strcmp(key, "use_tls") == 0 || strcmp(key, "tls") == 0) {
        // "1", "true", "on", "yes" → true / 그 외 → false
        c->use_tls = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
                      strcmp(value, "on") == 0 || strcmp(value, "yes") == 0);
    } else if (strcmp(key, "net_mode") == 0) {
        // 0=off, 1=socket, 2=mqtt
        int m = atoi(value);
        if (m < 0 || m > 2) m = 1;  // 기본값 SOCKET
        c->net_mode = (uint8_t)m;
    } else if (strcmp(key, "socket_host") == 0) {
        strncpy(c->socket_host, value, MAX_SOCKET_HOST_LEN);
        c->socket_host[MAX_SOCKET_HOST_LEN] = 0;
    } else if (strcmp(key, "socket_port") == 0) {
        c->socket_port = (uint16_t)atoi(value);
    } else if (strcmp(key, "net_proto") == 0) {
        // 0=tcp, 1=udp, 2=udp-bcast
        int p = atoi(value);
        if (p < 0 || p > 2) p = 0;  // 기본값 TCP
        c->net_proto = (uint8_t)p;
    }
}

// ----- NVS → SPIFFS 마이그레이션 (1회) -----
// 기존 wifi_bridge가 NVS에 저장한 ssid/pass를 SPIFFS로 옮김
#include <nvs.h>
#include <nvs_flash.h>
#define BC_WIFI_NVS_NS  "bwifi"
#define BC_WIFI_NVS_SSID "ssid"
#define BC_WIFI_NVS_PASS "pass"

static void migrate_nvs_to_spiffs(void) {
    nvs_handle_t h;
    if (nvs_open(BC_WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;  // NVS에 기존 데이터 없음 → 마이그레이션 불필요
    }

    char ssid[65] = {0,};
    char pass[65] = {0,};
    size_t ssid_sz = sizeof(ssid);
    size_t pass_sz = sizeof(pass);
    bool has_ssid = nvs_get_str(h, BC_WIFI_NVS_SSID, ssid, &ssid_sz) == ESP_OK;
    bool has_pass = nvs_get_str(h, BC_WIFI_NVS_PASS, pass, &pass_sz) == ESP_OK;
    nvs_close(h);

    if (!has_ssid) {
        return;  // NVS에 ssid 없음 → 마이그레이션 불필요
    }

    LOGI("node_config: migrating WiFi credentials from NVS → SPIFFS");

    // 현재 SPIFFS에 wifi.conf 없으면 마이그레이션 진행
    if (!SPIFFS.exists(WIFI_CONFIG_FILE)) {
        bc_wifi_config_t cfg;
        node_config_load_wifi(&cfg);  // 기본값 로드
        strncpy(cfg.ssid, ssid, MAX_WIFI_SSID_LEN);
        strncpy(cfg.pass, pass, MAX_WIFI_PASS_LEN);
        node_config_save_wifi(&cfg);
        LOGI("node_config: migrated ssid=%s to /wifi.conf", cfg.ssid);

        // NVS에서 삭제 (다음 부팅 시 마이그레이션 반복 안 함)
        if (nvs_open(BC_WIFI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, BC_WIFI_NVS_SSID);
            nvs_erase_key(h, BC_WIFI_NVS_PASS);
            nvs_commit(h);
            nvs_close(h);
            LOGI("node_config: erased old NVS WiFi keys");
        }
    }
}

// ----- public API -----

bool node_config_init(void) {
    if (g_init_done) return true;
    if (!SPIFFS.begin(true)) {
        LOGE("node_config: SPIFFS mount failed");
        return false;
    }
    g_init_done = true;

    // NVS → SPIFFS 마이그레이션 (1회, 자동)
    migrate_nvs_to_spiffs();

    // 캐시 로드
    node_profile_t prof;
    node_config_load_profile(&prof);

    bc_wifi_config_t wifi;
    node_config_load_wifi(&wifi);

    LOGI("node_config: initialized (nickname='%s', wifi ssid='%s', broker=%s:%u)",
         g_profile_cache.nickname,
         g_wifi_cache.ssid,
         g_wifi_cache.mqtt_broker,
         g_wifi_cache.mqtt_port);
    return true;
}

bool node_config_load_profile(node_profile_t* profile) {
    if (!profile) return false;
    memset(profile, 0, sizeof(*profile));

    bool ok = parse_config_file(NODE_CONFIG_FILE, profile_cb, profile);
    if (!ok) {
        // 파일 없음 - 빈 프로필 그대로
    }

    // 캐시 업데이트
    memcpy(&g_profile_cache, profile, sizeof(g_profile_cache));
    g_profile_loaded = true;
    return ok;
}

bool node_config_save_profile(const node_profile_t* profile) {
    if (!profile) return false;
    File f = SPIFFS.open(NODE_CONFIG_FILE, "w");
    if (!f) {
        LOGE("node_config: open(%s, w) failed", NODE_CONFIG_FILE);
        return false;
    }
    f.println("# bitchat relay node profile");
    f.println("# Lines starting with '#' or ';' are comments. Format: key=value");
    f.println("");
    f.print("nickname=");
    f.println(profile->nickname);
    f.close();

    // 캐시 업데이트
    memcpy(&g_profile_cache, profile, sizeof(g_profile_cache));
    LOGI("node_config: saved /node.conf (nickname='%s')", profile->nickname);
    return true;
}

const char* node_config_get_nickname(void) {
    if (!g_profile_loaded) {
        node_config_load_profile(&g_profile_cache);
    }
    return g_profile_cache.nickname;
}

bool node_config_set_nickname(const char* nickname) {
    node_profile_t p;
    node_config_load_profile(&p);  // 현재 값 로드
    if (nickname && *nickname) {
        strncpy(p.nickname, nickname, MAX_NICKNAME_LEN);
        p.nickname[MAX_NICKNAME_LEN] = 0;
    } else {
        p.nickname[0] = 0;  // nickname 제거
    }
    return node_config_save_profile(&p);
}

bool node_config_load_wifi(bc_wifi_config_t* cfg) {
    if (!cfg) return false;
    // 기본값으로 초기화
    memset(cfg, 0, sizeof(*cfg));
    cfg->ssid[0] = 0;
    cfg->pass[0] = 0;
    strncpy(cfg->mqtt_broker, BC_MQTT_BROKER, MAX_MQTT_BROKER_LEN);
    cfg->mqtt_port = BC_MQTT_PORT;
    strncpy(cfg->mqtt_topic_prefix, BC_MQTT_TOPIC_PREFIX, MAX_TOPIC_PREFIX_LEN);
    cfg->use_tls = false;  // 기본값: plain MQTT (port 1883)
    // v19: socket 설정 기본값
    cfg->net_mode = 1;     // 기본 SOCKET
    cfg->socket_host[0] = 0;  // 빈 문자열 (미설정)
    cfg->socket_port = 8765;  // 기본 포트
    // v20: socket protocol 기본값
    cfg->net_proto = 0;    // 기본 TCP

    parse_config_file(WIFI_CONFIG_FILE, wifi_cb, cfg);

    // use_tls=true인데 port가 1883(기본)이면 자동으로 8883으로 변경
    if (cfg->use_tls && cfg->mqtt_port == 1883) {
        cfg->mqtt_port = 8883;
    }

    // 캐시 업데이트
    memcpy(&g_wifi_cache, cfg, sizeof(g_wifi_cache));
    g_wifi_loaded = true;
    return true;
}

bool node_config_save_wifi(const bc_wifi_config_t* cfg) {
    if (!cfg) return false;
    File f = SPIFFS.open(WIFI_CONFIG_FILE, "w");
    if (!f) {
        LOGE("node_config: open(%s, w) failed", WIFI_CONFIG_FILE);
        return false;
    }
    f.println("# bitchat relay wifi/mqtt config");
    f.println("# Lines starting with '#' or ';' are comments. Format: key=value");
    f.println("");
    f.print("ssid=");
    f.println(cfg->ssid);
    f.print("pass=");
    f.println(cfg->pass);
    f.print("broker=");
    f.println(cfg->mqtt_broker);
    f.print("port=");
    f.println((unsigned)cfg->mqtt_port);
    f.print("topic_prefix=");
    f.println(cfg->mqtt_topic_prefix);
    f.print("use_tls=");
    f.println(cfg->use_tls ? "1" : "0");
    // v19: socket 설정 저장
    f.print("net_mode=");
    f.println((unsigned)cfg->net_mode);
    f.print("socket_host=");
    f.println(cfg->socket_host);
    f.print("socket_port=");
    f.println((unsigned)cfg->socket_port);
    // v20: socket protocol 저장
    f.print("net_proto=");
    f.println((unsigned)cfg->net_proto);
    f.close();

    // 캐시 업데이트
    memcpy(&g_wifi_cache, cfg, sizeof(g_wifi_cache));
    LOGI("node_config: saved /wifi.conf (ssid='%s', broker=%s:%u, tls=%s, net_mode=%u, socket=%s:%u)",
         cfg->ssid, cfg->mqtt_broker, cfg->mqtt_port,
         cfg->use_tls ? "ON" : "OFF",
         cfg->net_mode,
         cfg->socket_host[0] ? cfg->socket_host : "(none)", cfg->socket_port);
    return true;
}

const bc_wifi_config_t* node_config_get_wifi(void) {
    if (!g_wifi_loaded) {
        node_config_load_wifi(&g_wifi_cache);
    }
    return &g_wifi_cache;
}

bool node_config_set_wifi_credentials(const char* ssid, const char* pass) {
    bc_wifi_config_t cfg;
    node_config_load_wifi(&cfg);  // 현재 값 로드
    if (ssid) {
        strncpy(cfg.ssid, ssid, MAX_WIFI_SSID_LEN);
        cfg.ssid[MAX_WIFI_SSID_LEN] = 0;
    }
    if (pass) {
        strncpy(cfg.pass, pass, MAX_WIFI_PASS_LEN);
        cfg.pass[MAX_WIFI_PASS_LEN] = 0;
    }
    return node_config_save_wifi(&cfg);
}

bool node_config_set_mqtt_broker(const char* broker, uint16_t port) {
    bc_wifi_config_t cfg;
    node_config_load_wifi(&cfg);
    if (broker) {
        strncpy(cfg.mqtt_broker, broker, MAX_MQTT_BROKER_LEN);
        cfg.mqtt_broker[MAX_MQTT_BROKER_LEN] = 0;
    }
    if (port > 0) {
        cfg.mqtt_port = port;
    }
    return node_config_save_wifi(&cfg);
}

bool node_config_set_mqtt_topic_prefix(const char* prefix) {
    if (!prefix) return false;
    bc_wifi_config_t cfg;
    node_config_load_wifi(&cfg);
    strncpy(cfg.mqtt_topic_prefix, prefix, MAX_TOPIC_PREFIX_LEN);
    cfg.mqtt_topic_prefix[MAX_TOPIC_PREFIX_LEN] = 0;
    return node_config_save_wifi(&cfg);
}

// ----- TLS (mTLS MQTT) 설정 -----

bool node_config_set_tls_enabled(bool on) {
    bc_wifi_config_t cfg;
    node_config_load_wifi(&cfg);
    cfg.use_tls = on;
    // port 자동 조정: 1883 ↔ 8883
    if (on && cfg.mqtt_port == 1883) {
        cfg.mqtt_port = 8883;
    } else if (!on && cfg.mqtt_port == 8883) {
        cfg.mqtt_port = 1883;
    }
    return node_config_save_wifi(&cfg);
}

bool node_config_get_tls_enabled(void) {
    return node_config_get_wifi()->use_tls;
}

// 인증서 파일 경로 정의 (PEM 형식)
#define BROKER_CA_FILE   "/broker_ca.pem"
#define CLIENT_CERT_FILE "/client_cert.pem"
#define CLIENT_KEY_FILE  "/client_key.pem"

int node_config_check_certs(void) {
    int mask = 0;
    if (SPIFFS.exists(BROKER_CA_FILE))   mask |= 1;  // bit 0: CA
    if (SPIFFS.exists(CLIENT_CERT_FILE)) mask |= 2;  // bit 1: client cert
    if (SPIFFS.exists(CLIENT_KEY_FILE))  mask |= 4;  // bit 2: client key
    return mask;
}

// ===== v19: net transport mode + socket 설정 (SPIFFS 영속화) =====

bool node_config_set_net_mode(uint8_t mode) {
    if (mode > 2) mode = 1;  // 범위 클램프, 기본 SOCKET
    bc_wifi_config_t cfg;
    node_config_load_wifi(&cfg);
    cfg.net_mode = mode;
    return node_config_save_wifi(&cfg);
}

uint8_t node_config_get_net_mode(void) {
    return node_config_get_wifi()->net_mode;
}

bool node_config_set_socket_host(const char* host) {
    bc_wifi_config_t cfg;
    node_config_load_wifi(&cfg);
    if (host && host[0]) {
        strncpy(cfg.socket_host, host, MAX_SOCKET_HOST_LEN);
        cfg.socket_host[MAX_SOCKET_HOST_LEN] = 0;
    } else {
        cfg.socket_host[0] = 0;
    }
    return node_config_save_wifi(&cfg);
}

const char* node_config_get_socket_host(void) {
    return node_config_get_wifi()->socket_host;
}

bool node_config_set_socket_port(uint16_t port) {
    if (port == 0) port = 8765;
    bc_wifi_config_t cfg;
    node_config_load_wifi(&cfg);
    cfg.socket_port = port;
    return node_config_save_wifi(&cfg);
}

uint16_t node_config_get_socket_port(void) {
    return node_config_get_wifi()->socket_port;
}

// ===== v20: socket protocol (SPIFFS 영속화) =====

bool node_config_set_net_proto(uint8_t proto) {
    if (proto > 2) proto = 0;  // 범위 클램프, 기본 TCP
    bc_wifi_config_t cfg;
    node_config_load_wifi(&cfg);
    cfg.net_proto = proto;
    return node_config_save_wifi(&cfg);
}

uint8_t node_config_get_net_proto(void) {
    return node_config_get_wifi()->net_proto;
}
