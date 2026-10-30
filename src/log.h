/*
 * log.h - Serial logging helpers for bitchat-ESP32 relay
 *
 * 한국어: 시리얼 UART(115200bps)로 사람이 읽기 쉬운 텍스트 로그와
 *        패킷 hex dump를 출력하기 위한 매크로/인라인 함수들입니다.
 *
 * 주의: NimBLE 헤더가 LOG_LEVEL_ERROR 등을 매크로로 정의하므로
 *       BC_LOG_LEVEL_ 접두어 사용.
 */

#pragma once

#include <Arduino.h>
#include <inttypes.h>

#ifndef BC_LOG_LEVEL
#define BC_LOG_LEVEL BC_LOG_LEVEL_DEBUG
#endif

enum LogLevel {
    BC_LOG_LEVEL_ERROR = 0,
    BC_LOG_LEVEL_WARN  = 1,
    BC_LOG_LEVEL_INFO  = 2,
    BC_LOG_LEVEL_DEBUG = 3,
    BC_LOG_LEVEL_TRACE = 4
};

#define LOG_TAG "bitchat"

// 시간/레벨/태그 prefix
static inline const char* log_level_str(int lv) {
    switch (lv) {
        case BC_LOG_LEVEL_ERROR: return "E";
        case BC_LOG_LEVEL_WARN:  return "W";
        case BC_LOG_LEVEL_INFO:  return "I";
        case BC_LOG_LEVEL_DEBUG: return "D";
        case BC_LOG_LEVEL_TRACE: return "T";
        default:                  return "?";
    }
}

#define LOG_BASE(lvl, fmt, ...)                                              \
    do {                                                                     \
        if ((lvl) <= BC_LOG_LEVEL) {                                         \
            char _buf[256];                                                   \
            uint32_t _ms = millis();                                          \
            snprintf(_buf, sizeof(_buf),                                     \
                     "[%6lu.%03lu][%s][%s] " fmt "\r\n",                     \
                     (unsigned long)(_ms / 1000),                            \
                     (unsigned long)(_ms % 1000),                           \
                     log_level_str(lvl), LOG_TAG,                            \
                     ##__VA_ARGS__);                                         \
            Serial.print(_buf);                                              \
        }                                                                    \
    } while (0)

#define LOGE(fmt, ...) LOG_BASE(BC_LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_BASE(BC_LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) LOG_BASE(BC_LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) LOG_BASE(BC_LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOGT(fmt, ...) LOG_BASE(BC_LOG_LEVEL_TRACE, fmt, ##__VA_ARGS__)

// hex dump - 패킷 분석용 (v17: INFO 레벨로 올려 BC_LOG_LEVEL=2에서도 출력)
//   데이터 덤프는 디버깅 핵심 정보이므로 LOGD가 아닌 INFO로 출력.
//   나머지 LOGD (scan results, signature verify 등)는 BC_LOG_LEVEL=2에서 제거됨.
static inline void log_hexdump(const char* tag, const uint8_t* data, size_t len) {
    if (BC_LOG_LEVEL < BC_LOG_LEVEL_INFO) return;
    LOGI("[%s] len=%u", tag, (unsigned)len);
    const size_t per_line = 16;
    char line[160];
    size_t off = 0;
    while (off < len) {
        size_t remain = len - off;
        size_t this_line = (remain < per_line) ? remain : per_line;
        char* p = line;
        p += snprintf(p, sizeof(line) - (p - line), "%04zx  ", off);
        for (size_t i = 0; i < this_line; ++i) {
            p += snprintf(p, sizeof(line) - (p - line), "%02x ", data[off + i]);
        }
        for (size_t i = this_line; i < per_line; ++i) {
            p += snprintf(p, sizeof(line) - (p - line), "   ");
        }
        p += snprintf(p, sizeof(line) - (p - line), " |");
        for (size_t i = 0; i < this_line; ++i) {
            uint8_t c = data[off + i];
            p += snprintf(p, sizeof(line) - (p - line), "%c",
                          (c >= 32 && c < 127) ? (char)c : '.');
        }
        p += snprintf(p, sizeof(line) - (p - line), "|");
        Serial.print(line);
        Serial.print("\r\n");
        off += this_line;
    }
}
