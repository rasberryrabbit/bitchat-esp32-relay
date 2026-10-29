#!/bin/bash
# gen_client_cert.sh - bitchat relay 클라이언트 인증서 발급 (CA 서명)
#
# 사용:
#   ./gen_client_cert.sh <name> [days]
#
# 예:
#   ./gen_client_cert.sh esp32-relay-livingroom
#   → esp32-relay-livingroom.key, .csr, .crt 생성
#   → broker_ca.pem, client_cert.pem, client_key.pem (ESP32 업로드용)
#
#   ./gen_client_cert.sh esp32-relay-balcony 730
#   → 730일(2년) 유효기간으로 발급 (기본 365일)
#
# 사전 조건:
#   - ./ca.key, ./ca.crt가 존재해야 함 (./gen_ca.sh 먼저 실행)
#   - openssl.cnf 파일이 같은 디렉터리에 있어야 함
#
# 출력:
#   <name>.key  - 클라이언트 개인키 (절대 비공개, ESP32에만)
#   <name>.csr  - 인증서 서명 요청 (발급 후 불필요, 삭제 가능)
#   <name>.crt  - 클라이언트 인증서 (CA 서명됨)
#   broker_ca.pem   - ca.crt의 복사본 (ESP32 업로드용)
#   client_cert.pem - <name>.crt의 복사본 (ESP32 업로드용)
#   client_key.pem  - <name>.key의 복사본 (ESP32 업로드용)
#
# ESP32 업로드 (Serial 명령):
#   cert upload ca     # broker_ca.pem 내용 붙여넣기 (EOF로 종료)
#   cert upload client # client_cert.pem 내용 붙여넣기
#   cert upload key    # client_key.pem 내용 붙여넣기
#   wifi tls on

set -e
cd "$(dirname "$0")"

CONFIG=openssl.cnf
CA_KEY=ca.key
CA_CRT=ca.crt
CA_SRL=ca.srl

if [ $# -lt 1 ]; then
    echo "Usage: $0 <name> [days]"
    echo "  name: 클라이언트 식별자 (예: esp32-relay-livingroom)"
    echo "  days: 유효기간 (기본 365일)"
    exit 1
fi

NAME="$1"
DAYS="${2:-365}"

if [ ! -f "$CONFIG" ] || [ ! -f "$CA_KEY" ] || [ ! -f "$CA_CRT" ]; then
    echo "ERROR: CA files not found. Run ./gen_ca.sh first."
    echo "  expected: $CONFIG, $CA_KEY, $CA_CRT"
    exit 1
fi

CLIENT_KEY="${NAME}.key"
CLIENT_CSR="${NAME}.csr"
CLIENT_CRT="${NAME}.crt"

if [ -f "$CLIENT_KEY" ] || [ -f "$CLIENT_CRT" ]; then
    echo "ERROR: $CLIENT_KEY or $CLIENT_CRT already exists"
    echo "  → 기존 인증서 백업 후 삭제: mv $CLIENT_KEY ${CLIENT_KEY}.bak && mv $CLIENT_CRT ${CLIENT_CRT}.bak"
    exit 1
fi

echo "=== Generating client cert: $NAME (valid $DAYS days) ==="
echo ""

# 클라이언트 개인키 생성
echo "[1/4] Generating client private key (RSA 2048)..."
openssl genrsa -out "$CLIENT_KEY" 2048 2>/dev/null
chmod 600 "$CLIENT_KEY"
echo "  → $CLIENT_KEY"

# CSR 생성 (CN = name)
echo "[2/4] Generating CSR (CN=$NAME)..."
# CN만 입력받도록 subject 인라인 지정
openssl req -new -key "$CLIENT_KEY" -out "$CLIENT_CSR" \
    -subj "/C=KR/O=bitchat-relay/OU=clients/CN=$NAME" \
    -config "$CONFIG" 2>/dev/null
echo "  → $CLIENT_CSR"

# CA로 서명
echo "[3/4] Signing CSR with CA (valid $DAYS days)..."
openssl x509 -req -in "$CLIENT_CSR" \
    -CA "$CA_CRT" -CAkey "$CA_KEY" -CAserial "$CA_SRL" \
    -out "$CLIENT_CRT" -days "$DAYS" \
    -extfile "$CONFIG" -extensions usr_cert 2>/dev/null
echo "  → $CLIENT_CRT"

# ESP32 업로드용 PEM 파일 복사 (이름 통일)
echo "[4/4] Copying to ESP32-ready filenames..."
cp "$CA_CRT"     broker_ca.pem
cp "$CLIENT_CRT" client_cert.pem
cp "$CLIENT_KEY" client_key.pem
chmod 600 client_key.pem broker_ca.pem client_cert.pem
echo "  → broker_ca.pem   (CA cert, for ESP32 /broker_ca.pem)"
echo "  → client_cert.pem (client cert, for ESP32 /client_cert.pem)"
echo "  → client_key.pem  (client private key, for ESP32 /client_key.pem)"

echo ""
echo "=== Client cert issued successfully ==="
echo ""
echo "Client cert info:"
openssl x509 -in "$CLIENT_CRT" -noout -subject -dates -issuer
echo ""
echo "Serial number:"
openssl x509 -in "$CLIENT_CRT" -noout -serial
echo ""
echo "Fingerprint (SHA-256):"
openssl x509 -in "$CLIENT_CRT" -noout -fingerprint -sha256 | sed 's/^/  /'
echo ""
echo "=== Next steps ==="
echo "1. Upload PEM files to ESP32 (via Serial Monitor):"
echo "   > cert upload ca"
echo "   (paste contents of broker_ca.pem, end with 'EOF' line)"
echo ""
echo "   > cert upload client"
echo "   (paste contents of client_cert.pem, end with 'EOF' line)"
echo ""
echo "   > cert upload key"
echo "   (paste contents of client_key.pem, end with 'EOF' line)"
echo ""
echo "2. Verify cert status:"
echo "   > cert status"
echo "   (should show mask=0x7 mTLS ready)"
echo ""
echo "3. Enable TLS:"
echo "   > wifi tls on"
echo "   > wifi on"
echo ""
echo "=== Security ==="
echo "- $CLIENT_KEY and client_key.pem MUST stay private"
echo "- Back up $CA_KEY, $CA_CRT, $CA_SRL to safe storage"
echo "- Track expiry: ./check_cert_expiry.sh $CLIENT_CRT"
