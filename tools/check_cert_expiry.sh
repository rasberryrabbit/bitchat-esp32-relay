#!/bin/bash
# check_cert_expiry.sh - 인증서 만료일 확인
#
# 사용:
#   ./check_cert_expiry.sh <cert.crt>
#   ./check_cert_expiry.sh <cert.crt> [warn_days]
#
# 예:
#   ./check_cert_expiry.sh esp32-relay-livingroom.crt
#   → 만료일 출력, 30일 이내면 경고
#
#   ./check_cert_expiry.sh esp32-relay-livingroom.crt 60
#   → 60일 이내면 경고
#
# 종료 코드:
#   0 = 정상 (만료까지 warn_days 이상 남음)
#   1 = 만료 임박 (warn_days 이내)
#   2 = 이미 만료
#   3 = 인증서 파일 없음 / 오류
#
# cron 자동화:
#   0 9 * * * /path/to/tools/check_cert_expiry.sh /path/to/esp32-relay.crt 30 || \
#             mail -s "ESP32 cert expiring soon" admin@example.com < /dev/null

set -e

if [ $# -lt 1 ]; then
    echo "Usage: $0 <cert.crt> [warn_days]"
    echo "  warn_days: 만료 임박 경고 임계값 (기본 30일)"
    exit 3
fi

CERT="$1"
WARN_DAYS="${2:-30}"

if [ ! -f "$CERT" ]; then
    echo "ERROR: certificate not found: $CERT"
    exit 3
fi

# 인증서 만료일 추출 (OpenSSL 3.x 호환)
EXPIRY_DATE=$(openssl x509 -in "$CERT" -noout -enddate | cut -d= -f2)
if [ -z "$EXPIRY_DATE" ]; then
    echo "ERROR: cannot read expiry date from $CERT"
    exit 3
fi

# Unix timestamp로 변환
EXPIRY_TS=$(date -d "$EXPIRY_DATE" +%s 2>/dev/null || date -j -f "%b %d %H:%M:%S %Y %Z" "$EXPIRY_DATE" +%s 2>/dev/null)
NOW_TS=$(date +%s)
SECONDS_LEFT=$((EXPIRY_TS - NOW_TS))
DAYS_LEFT=$((SECONDS_LEFT / 86400))

# 인증서 정보 출력
SUBJECT=$(openssl x509 -in "$CERT" -noout -subject | sed 's/^subject=//')
SERIAL=$(openssl x509 -in "$CERT" -noout -serial | cut -d= -f2)

echo "=== Certificate info ==="
echo "  File:      $CERT"
echo "  Subject:   $SUBJECT"
echo "  Serial:    $SERIAL"
echo "  Expires:   $EXPIRY_DATE"
echo "  Days left: $DAYS_LEFT"
echo ""

# 만료 상태 판정
if [ $DAYS_LEFT -lt 0 ]; then
    echo "❌ EXPIRED ($((-DAYS_LEFT)) days ago)"
    echo "   → 즉시 ./rotate_certs.sh로 새 인증서 발급 후 ESP32에 업로드"
    exit 2
elif [ $DAYS_LEFT -lt $WARN_DAYS ]; then
    echo "⚠️  EXPIRING SOON (within $WARN_DAYS days)"
    echo "   → ./rotate_certs.sh로 새 인증서 발급 후 ESP32에 업로드"
    echo "   → 또는 ./gen_client_cert.sh <name> $((DAYS_LEFT + 365))로 갱신"
    exit 1
else
    echo "✓ OK (more than $WARN_DAYS days remaining)"
    exit 0
fi
