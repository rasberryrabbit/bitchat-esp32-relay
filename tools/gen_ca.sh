#!/bin/bash
# gen_ca.sh - bitchat relay 자체 CA 생성
#
#   - ca.key (CA 개인키, 절대 비공개)
#   - ca.crt (CA 인증서, broker와 client에게 배포)
#   - ca.srl (시리얼 넘버 추적 파일)
#
# 사용:
#   ./gen_ca.sh
#   → 현재 디렉터리에 ca.key, ca.crt, ca.srl 생성
#
# 이후:
#   - ca.crt를 MQTT broker의 설정에 추가 (broker_ca.pem으로 복사)
#   - ./gen_client_cert.sh로 클라이언트 인증서 발급
#   - ESP32의 SPIFFS에 /broker_ca.pem으로 업로드

set -e
cd "$(dirname "$0")"

CONFIG=openssl.cnf
CA_KEY=ca.key
CA_CRT=ca.crt
CA_SRL=ca.srl

if [ ! -f "$CONFIG" ]; then
    echo "ERROR: $CONFIG not found in current directory"
    exit 1
fi

if [ -f "$CA_KEY" ] || [ -f "$CA_CRT" ]; then
    echo "ERROR: $CA_KEY or $CA_CRT already exists"
    echo "  → 기존 CA를 삭제하려면: rm -f $CA_KEY $CA_CRT $CA_SRL"
    echo "  → CA를 삭제하면 기존 클라이언트 인증서도 무효화됨 (rotation 필요)"
    exit 1
fi

echo "=== Generating bitchat relay CA ==="
echo ""

# CA 개인키 생성 (RSA 2048, 암호 없음 - 자동화 목적)
# 보안 강화하려면 -aes256 추가하여 passphrase 설정
echo "[1/3] Generating CA private key (RSA 2048, no passphrase)..."
openssl genrsa -out "$CA_KEY" 2048 2>/dev/null
chmod 600 "$CA_KEY"
echo "  → $CA_KEY (keep this PRIVATE)"

# CA 자체 서명 인증서 생성 (유효기간 10년)
echo "[2/3] Generating self-signed CA certificate (valid 10 years)..."
openssl req -new -x509 -key "$CA_KEY" -out "$CA_CRT" \
    -days 3650 -config "$CONFIG" -extensions v3_ca 2>/dev/null
echo "  → $CA_CRT (distribute this to broker + clients)"

# 시리얼 넘버 파일 (클라이언트 인증서 발급 시 자동 증가)
echo "[3/3] Initializing serial number file..."
echo "1000" > "$CA_SRL"
echo "  → $CA_SRL"

echo ""
echo "=== CA created successfully ==="
echo ""
echo "CA cert info:"
openssl x509 -in "$CA_CRT" -noout -subject -dates -issuer
echo ""
echo "Next steps:"
echo "  1. Deploy ca.crt to MQTT broker (as broker CA)"
echo "  2. Copy ca.crt → broker_ca.pem (for ESP32 SPIFFS upload)"
echo "  3. Run ./gen_client_cert.sh <name> to issue client certs"
echo ""
echo "Security notes:"
echo "  - ca.key MUST stay private (anyone with ca.key can sign certs)"
echo "  - Store ca.key in a safe place (encrypted backup recommended)"
echo "  - CA cert validity: 10 years (override via -days flag if needed)"
