#!/bin/bash
# rotate_certs.sh - 클라이언트 인증서 회전 (rotation)
#
# 기존 인증서를 백업하고 새 인증서를 발급하여 무효화를 방지.
# 유효기간 만료 30일 이내 또는 사고 대응 시 사용.
#
# 사용:
#   ./rotate_certs.sh <name> [days]
#
# 예:
#   ./rotate_certs.sh esp32-relay-livingroom
#   → 기존 esp32-relay-livingroom.crt를 .bak.N 으로 백업
#   → 새 인증서 발급 (기본 365일)
#   → broker_ca.pem, client_cert.pem, client_key.pem 갱신
#
#   ./rotate_certs.sh esp32-relay-livingroom 730
#   → 2년(730일) 유효기간으로 발급
#
# 절차:
#   1. 기존 .crt, .key 백업 (.bak.YYYYMMDD-HHMMSS 확장자)
#   2. gen_client_cert.sh로 새 인증서 발급
#   3. 사용자에게 업로드 절차 안내 (Serial 명령)
#   4. 새 인증서 fingerprint 출력 (확인용)
#
# 복구 (rollback):
#   - 새 인증서가 문제 있으면 백업에서 복구:
#     mv esp32-relay-livingroom.crt.bak.20240101 esp32-relay-livingroom.crt
#     mv esp32-relay-livingroom.key.bak.20240101 esp32-relay-livingroom.key
#     cp esp32-relay-livingroom.crt client_cert.pem
#     cp esp32-relay-livingroom.key client_key.pem

set -e
cd "$(dirname "$0")"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <name> [days]"
    echo "  name: 기존 클라이언트 인증서 이름 (확장자 제외)"
    echo "  days: 새 인증서 유효기간 (기본 365일)"
    exit 1
fi

NAME="$1"
DAYS="${2:-365}"

OLD_CRT="${NAME}.crt"
OLD_KEY="${NAME}.key"
OLD_CSR="${NAME}.csr"

# 기존 인증서 존재 확인
if [ ! -f "$OLD_CRT" ] || [ ! -f "$OLD_KEY" ]; then
    echo "ERROR: existing cert not found: $OLD_CRT / $OLD_KEY"
    echo "  → ./gen_client_cert.sh로 신규 발급하세요"
    exit 1
fi

# CA 존재 확인
if [ ! -f ca.key ] || [ ! -f ca.crt ]; then
    echo "ERROR: CA files (ca.key, ca.crt) not found"
    echo "  → ./gen_ca.sh로 CA를 먼저 생성하세요"
    exit 1
fi

# 기존 인증서 정보 표시
echo "=== Existing cert (to be rotated) ==="
openssl x509 -in "$OLD_CRT" -noout -subject -dates -serial 2>&1 | sed 's/^/  /'
echo ""

# 백업 (타임스탬프付き)
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
BACKUP_CRT="${OLD_CRT}.bak.${TIMESTAMP}"
BACKUP_KEY="${OLD_KEY}.bak.${TIMESTAMP}"

echo "=== Backup ==="
echo "  Backing up: $OLD_CRT → $BACKUP_CRT"
echo "  Backing up: $OLD_KEY → $BACKUP_KEY"
cp "$OLD_CRT" "$BACKUP_CRT"
cp "$OLD_KEY" "$BACKUP_KEY"
chmod 600 "$BACKUP_KEY"

# CSR은 재발급 시 다시 만들어지므로 삭제
rm -f "$OLD_CSR"

echo ""
echo "=== Generating new cert (valid $DAYS days) ==="

# gen_client_cert.sh 호출 (백업 후 기존 파일 삭제해야 함)
# 백업한 상태에서 기존 .crt, .key 삭제 후 재발급
rm "$OLD_CRT" "$OLD_KEY"

# 스크립트 직접 실행 (gen_client_cert.sh 인라인 구현)
CONFIG=openssl.cnf
CA_KEY=ca.key
CA_CRT=ca.crt
CA_SRL=ca.srl

# 새 클라이언트 개인키
openssl genrsa -out "$OLD_KEY" 2048 2>/dev/null
chmod 600 "$OLD_KEY"

# CSR
openssl req -new -key "$OLD_KEY" -out "$OLD_CSR" \
    -subj "/C=KR/O=bitchat-relay/OU=clients/CN=$NAME" \
    -config "$CONFIG" 2>/dev/null

# CA 서명
openssl x509 -req -in "$OLD_CSR" \
    -CA "$CA_CRT" -CAkey "$CA_KEY" -CAserial "$CA_SRL" \
    -out "$OLD_CRT" -days "$DAYS" \
    -extfile "$CONFIG" -extensions usr_cert 2>/dev/null

# ESP32 업로드용 PEM 갱신
cp "$CA_CRT"     broker_ca.pem
cp "$OLD_CRT"    client_cert.pem
cp "$OLD_KEY"    client_key.pem
chmod 600 client_key.pem broker_ca.pem client_cert.pem

echo "  → $OLD_KEY (new private key)"
echo "  → $OLD_CRT (new cert, valid $DAYS days)"
echo "  → broker_ca.pem (refreshed)"
echo "  → client_cert.pem (refreshed)"
echo "  → client_key.pem (refreshed)"

echo ""
echo "=== New cert info ==="
openssl x509 -in "$OLD_CRT" -noout -subject -dates -serial 2>&1 | sed 's/^/  /'
echo ""
echo "  Fingerprint (SHA-256):"
openssl x509 -in "$OLD_CRT" -noout -fingerprint -sha256 2>&1 | sed 's/^/  /'

echo ""
echo "=== Rotation complete ==="
echo ""
echo "Backup files retained:"
ls -la "${OLD_CRT}.bak."* "${OLD_KEY}.bak."* 2>/dev/null | awk '{print "  "$NF}' | sort
echo ""
echo "⚠️  ACTION REQUIRED: upload new PEMs to ESP32"
echo ""
echo "  Step 1 - Clear old certs on ESP32 (if loaded):"
echo "    > cert clear"
echo ""
echo "  Step 2 - Upload new CA cert:"
echo "    > cert upload ca"
echo "    (paste contents of broker_ca.pem, end with 'EOF' line)"
echo ""
echo "  Step 3 - Upload new client cert:"
echo "    > cert upload client"
echo "    (paste contents of client_cert.pem, end with 'EOF' line)"
echo ""
echo "  Step 4 - Upload new client key:"
echo "    > cert upload key"
echo "    (paste contents of client_key.pem, end with 'EOF' line)"
echo ""
echo "  Step 5 - Verify:"
echo "    > cert status   (mask=0x7 mTLS ready)"
echo "    > wifi on       (should connect via TLS now)"
echo ""
echo "Rollback (if new cert causes issues):"
echo "  mv $BACKUP_CRT $OLD_CRT"
echo "  mv $BACKUP_KEY $OLD_KEY"
echo "  cp $OLD_CRT client_cert.pem"
echo "  cp $OLD_KEY client_key.pem"
echo "  → then re-upload client_cert.pem + client_key.pem to ESP32"
