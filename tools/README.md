# bitchat ESP32 relay - mTLS 인증서 관리 도구

OpenSSL 기반 자체 CA 구축 + 클라이언트 인증서 발급 + 만료 관리 스크립트 모음.

## 파일 구성

| 파일 | 용도 |
|------|------|
| `openssl.cnf` | OpenSSL 설정 (CA / 클라이언트 / 서버 인증서 확장 정의) |
| `gen_ca.sh` | 자체 CA 생성 (ca.key, ca.crt, ca.srl) |
| `gen_client_cert.sh <name> [days]` | 클라이언트 인증서 발급 (CA 서명) |
| `check_cert_expiry.sh <cert> [warn_days]` | 인증서 만료일 확인 (cron 자동화 가능) |
| `rotate_certs.sh <name> [days]` | 기존 인증서 회전 (백업 + 새 발급) |

## 빠른 시작

```bash
cd tools/

# 1. 자체 CA 생성 (한 번만)
./gen_ca.sh
#   → ca.key (개인키, 비공개)
#   → ca.crt (인증서, broker + ESP32에 배포)
#   → ca.srl (시리얼 추적)

# 2. 클라이언트 인증서 발급
./gen_client_cert.sh esp32-relay-livingroom 365
#   → esp32-relay-livingroom.key / .crt
#   → broker_ca.pem / client_cert.pem / client_key.pem (ESP32 업로드용)

# 3. ESP32에 업로드 (Serial Monitor)
#   cert upload ca      # broker_ca.pem 내용 붙여넣기, EOF로 종료
#   cert upload client  # client_cert.pem 내용 붙여넣기, EOF로 종료
#   cert upload key     # client_key.pem 내용 붙여넣기, EOF로 종료
#   cert status         # mask=0x7 mTLS ready 확인
#   wifi tls on         # mTLS 활성화 (port 8883)
#   wifi on             # MQTT 연결

# 4. 만료일 모니터링 (cron 추천)
./check_cert_expiry.sh esp32-relay-livingroom.crt 30
#   → 30일 이내 만료 시 경고 (exit code 1)
#   → 이미 만료 시 exit code 2

# 5. 회전 (유효기간 만료 30일 이내 시)
./rotate_certs.sh esp32-relay-livingroom 365
#   → 기존 인증서 .bak.YYYYMMDD-HHMMSS 확장자로 백업
#   → 새 인증서 발급 + ESP32 업로드 절차 안내
```

## 인증서 체인 구조

```
CA (10년 유효)
├── esp32-relay-livingroom.crt (365일)
├── esp32-relay-balcony.crt (365일)
└── esp32-relay-bedroom.crt (730일)
```

- **CA (ca.crt)**: 자체 서명, 10년 유효. 모든 클라이언트 인증서를 서명.
- **클라이언트 인증서**: CA로 서명, 발급 시 유효기간 지정 (기본 365일).
- **브로커 인증서**: (옵션) CA로 서명, `openssl.cnf`의 `[server_cert]` 확장 사용.

## 자체 CA 구축 절차 (상세)

### 1단계: CA 생성

```bash
./gen_ca.sh
```

출력물:
- `ca.key`: CA 개인키 (RSA 2048, 비밀번호 없음). **절대 비공개**.
- `ca.crt`: CA 자체 서명 인증서. 유효기간 10년 (3650일).
- `ca.srl`: 시리얼 넘버 추적 파일. 자동 증가.

CA 인증서 subject:
```
C=KR, O=bitchat-relay, OU=self-signed CA, CN=bitchat-relay CA
```
(변경하려면 `openssl.cnf`의 `[req_distinguished_name]` 섹션 수정)

### 2단계: 클라이언트 인증서 발급

```bash
./gen_client_cert.sh <name> [days]
```

예:
```bash
./gen_client_cert.sh esp32-relay-livingroom         # 1년 (기본)
./gen_client_cert.sh esp32-relay-balcony 730        # 2년
./gen_client_cert.sh esp32-relay-bedroom 1095      # 3년
```

각 발급 시:
- `<name>.key`: 클라이언트 개인키 (RSA 2048)
- `<name>.csr`: 인증서 서명 요청 (발급 후 삭제 가능)
- `<name>.crt`: CA 서명된 클라이언트 인증서
- `broker_ca.pem`: ca.crt 복사본 (ESP32 `/broker_ca.pem`용)
- `client_cert.pem`: `<name>.crt` 복사본 (ESP32 `/client_cert.pem`용)
- `client_key.pem`: `<name>.key` 복사본 (ESP32 `/client_key.pem`용)

시리얼 넘버는 `ca.srl`에서 자동 증가 (1001, 1002, ...).

### 3단계: MQTT 브로커 설정 (예: Mosquitto)

`mosquitto.conf`에 CA와 클라이언트 인증 검증 추가:

```
listener 8883
protocol mqtt

cafile   /path/to/ca.crt
certfile /path/to/broker.crt
keyfile  /path/to/broker.key

require_certificate true   # mTLS (클라이언트 인증서 필수)
```

브로커 인증서는 별도 발급 (서버용 확장 사용):
```bash
openssl genrsa -out broker.key 2048
openssl req -new -key broker.key -out broker.csr \
    -subj "/C=KR/O=bitchat-relay/OU=brokers/CN=broker.example.com" \
    -config openssl.cnf
openssl x509 -req -in broker.csr \
    -CA ca.crt -CAkey ca.key -CAserial ca.srl \
    -out broker.crt -days 730 \
    -extfile openssl.cnf -extensions server_cert
```

### 4단계: ESP32에 인증서 업로드

Serial Monitor에서:
```
> cert upload ca
cert: paste PEM content for /broker_ca.pem
cert: end with a single line containing only 'EOF' (or send empty line)
-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIUTLrM...
(여기에 broker_ca.pem 내용 붙여넣기)
-----END CERTIFICATE-----
EOF
cert: upload done (1234 bytes, 22 lines) → /broker_ca.pem
```

같은 방법으로:
```
> cert upload client    # client_cert.pem 내용
> cert upload key       # client_key.pem 내용
```

확인:
```
> cert status
=== cert status ===
  /broker_ca.pem   : OK
  /client_cert.pem : OK
  /client_key.pem  : OK
  mask=0x7 (mTLS ready mode)
  use_tls=OFF (current wifi.conf)
```

활성화:
```
> wifi tls on
> wifi on
```

## 인증서 만료 관리

### 만료일 확인

```bash
./check_cert_expiry.sh esp32-relay-livingroom.crt
#   30일 이내 만료 시 exit code 1 (경고)
#   이미 만료 시 exit code 2 (긴급)
```

인자:
- `<cert>`: 검사할 .crt 파일
- `[warn_days]`: 경고 임계값 (기본 30일)

### cron 자동화 (매일 09:00 검사)

```bash
# /etc/cron.d/bitchat-cert-check
0 9 * * * user /path/to/tools/check_cert_expiry.sh /path/to/esp32-relay.crt 30 \
    || mail -s "ESP32 cert expiring soon" admin@example.com < /dev/null
```

또는 systemd timer / logrotate-style cron job으로 자동 알림.

### 회전 (rotation) 절차

만료 30일 이내 또는 보안 사고 시 새 인증서로 교체:

```bash
./rotate_certs.sh esp32-relay-livingroom 365
```

절차:
1. 기존 `<name>.crt`, `<name>.key`를 `.bak.YYYYMMDD-HHMMSS` 확장자로 백업
2. 새 개인키 + CSR 생성
3. CA로 서명하여 새 인증서 발급
4. `broker_ca.pem`, `client_cert.pem`, `client_key.pem` 갱신
5. ESP32 업로드 절차 안내 출력 (Serial 명령)
6. 롤백(rollback) 방법 안내

**백업 파일은 자동 삭제되지 않음** → 직접 관리 필요:
```bash
# 1주일 지난 백업 삭제
find . -name "*.bak.*" -mtime +7 -delete

# 또는 보관을 위해 tar로 묶어서 백업
tar czf cert-backups-$(date +%Y%m%d).tar.gz *.bak.*
```

### 롤백 (rollback)

새 인증서가 문제를 일으키면 백업에서 복구:

```bash
./rotate_certs.sh esp32-relay-livingroom 365
# ... 문제 발생 ...

# 롤백
mv esp32-relay-livingroom.crt.bak.20240101-090000 esp32-relay-livingroom.crt
mv esp32-relay-livingroom.key.bak.20240101-090000 esp32-relay-livingroom.key
cp esp32-relay-livingroom.crt client_cert.pem
cp esp32-relay-livingroom.key client_key.pem

# ESP32에 재업로드
# > cert upload client
# > cert upload key
```

## 보안 고려사항

### 파일 보호

| 파일 | 민감도 | 보호 방법 |
|------|--------|-----------|
| `ca.key` | **CRITICAL** | 600 perm, 암호화된 백업, 접근 제어 |
| `*.key` (클라이언트) | **CRITICAL** | 600 perm, ESP32에만 업로드, git에 넣지 않음 |
| `ca.crt` | 공개 가능 | broker + 모든 ESP32에 배포 |
| `*.crt` (클라이언트) | 공개 가능 | broker 검증용 |
| `*.csr` | 폐기 가능 | 발급 후 삭제 (`rm -f *.csr`) |
| `ca.srl` | 보관 필요 | 시리얼 추적, CA 서명 시 사용 |
| `*.bak.*` | 기존 key 포함 | 600 perm, 정기 삭제 권장 |

### `.gitignore` 추천

```
tools/ca.key
tools/ca.srl
tools/*.key
tools/*.pem
tools/*.bak.*
```

CA 인증서(`ca.crt`)와 클라이언트 인증서(`*.crt`)는 공개 가능하나 git에 넣기 전 신중하게 검토.

### CA 키 유출 시 대응

1. `ca.key`가 유출되면 공격자가 임의의 클라이언트 인증서를 발급할 수 있음
2. 즉시 모든 클라이언트 인증서 무효화:
   - MQTT broker의 `cafile`을 새 CA로 교체
   - 모든 ESP32에 새 CA + 새 클라이언트 인증서 업로드
3. 새 CA 생성: `./gen_ca.sh` (기존 ca.key/crt는 안전한 곳에 격리)
4. 모든 클라이언트 재발급: 각 ESP32마다 `./gen_client_cert.sh <name>`

## 백업 전략

### CA + 모든 클라이언트 인증서 백업

```bash
# 매월 1일 백업 (cron)
0 0 1 * * tar czf /backup/bitchat-certs-$(date +\%Y\%m).tar.gz \
    tools/ca.key tools/ca.crt tools/ca.srl \
    tools/esp32-relay-*.key tools/esp32-relay-*.crt \
    --mode=600
```

### 복구

```bash
tar xzf /backup/bitchat-certs-202401.tar.gz -C /path/to/project/
# 또는 새 머신에 복구 후 같은 CA로 클라이언트 인증서 추가 발급 가능
```

## 문제 해결

### TLS handshake 실패

1. ESP32 로그 확인:
   ```
   wifi_bridge: loaded broker CA from /broker_ca.pem
   mqtt: connecting to broker broker.example.com:8883 ...
   [E][WiFiClientSecure.cpp:XXX] connect(): ... handshake failed
   ```
2. 원인 진단:
   - CA 불일치: 브로커의 ca.crt와 ESP32의 broker_ca.pem이 다름
   - 시간 동기화: ESP32의 RTC가 브로커 인증서 유효기간 밖 (NTP 설정 필요)
   - SNI 불일치: 브로커가 SNI(Server Name Indication) 요구 → 호스트명 확인
   - 인증서 만료: `./check_cert_expiry.sh *.crt`

3. 디버깅:
   - `openssl s_client -connect broker:8883 -CAfile ca.crt -cert client.crt -key client.key`
   - 에러 메시지에서 정확한 원인 파악

### 클라이언트 인증서 미인증

브로커 로그에 "client certificate not found" 또는 "verification failed":
- `require_certificate true` 설정 확인 (mosquitto.conf)
- broker_ca.pem이 브로커의 cafile과 동일한지 확인
- 클라이언트 인증서의 issuer가 CA subject와 일치하는지 확인:
  ```bash
  openssl x509 -in client.crt -noout -issuer
  openssl x509 -in ca.crt -noout -subject
  # 둘이 일치해야 함
  ```

### 시간 동기화 (ESP32 RTC)

ESP32에 RTC가 없어 재부팅 시 시간이 초기화 → TLS 인증서 유효기간 검증 실패 가능.

해결책:
1. SNTP로 부팅 시 시간 동기화
2. 또는 NTP 서버에서 시간 가져오기

코드 예시 (wifi_bridge.cpp에 추가 가능):
```cpp
#include <time.h>
configTime(0, 0, "pool.ntp.org");
time_t now = time(nullptr);
while (now < 8 * 3600 * 365) {  // 1970년 이후 1년까지 대기
    delay(500);
    now = time(nullptr);
}
LOGI("time synced: %s", ctime(&now));
```
