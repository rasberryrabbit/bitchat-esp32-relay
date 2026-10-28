# bitchat ESP32 Relay Firmware

> ESP32 클래식 / ESP32-C3 기반의 bitchat 메시 중계 노드 펌웨어

이 펌웨어는 ESP32를 bitchat BLE 메시의 중계(relay) 노드로 동작시킵니다.
주변 bitchat BLE 노드(스마트폰, 다른 ESP32, 등)가 보낸 패킷을 수신해서
서명을 검증한 뒤 다른 인접 노드로 전달합니다. 인터넷이나 WiFi 없이
오프라인으로 동작하며, 메시 커버리지를 확장합니다.

---

## 지원 보드

| 항목 | ESP32 클래식 | ESP32-C3 |
|------|--------------|----------|
| 코어 | Xtensa LX6 듀얼코어 240MHz | RISC-V 싱글코어 160MHz |
| SRAM | 520 KB | 400 KB |
| Flash | 4 MB 이상 (보드에 따라) | 4 MB (일반적) |
| BLE | 4.2 (Bluedroid 기본) | 5.0 (NimBLE 기본, 본 펌웨어는 Bluedroid 강제) |
| WiFi | 802.11 b/g/n | 802.11 b/g/n (WiFi 4) |
| 동시 BLE 연결 (본 펌웨어) | 7 (incoming 4 + outgoing 3) | 4 (incoming 2 + outgoing 2) |
| 전원 | USB 5V 상시 | USB 5V 상시 |
| 추천 용도 | 메시 허브 (고처리량) | 저전력 소형 노드 / 배터리 운용 |

두 보드 모두 동일한 소스 코드로 빌드되며, 빌드 시 `BOARD_VARIANT_*` 매크로로
연결 수와 큐 크기가 자동 조정됩니다.

---

## 주요 기능

| 항목 | 구현 내용 |
|------|----------|
| BLE GATT 서버 | 자체 서비스 UUID(`0000bc00-...`)로 advertise, 클라이언트 write 수신 |
| BLE GATT 클라이언트 | 동시 스캐너로 주변 bitchat 노드 발견 시 자동 연결 + notify 구독 |
| Ed25519 서명 검증 | mbedtls 기반, 모든 수신 패킷 서명 검증 후에만 중계 |
| NVS 영속화 | 자체 Ed25519 키쌍, 카운터, 피어 목록을 플래시에 저장 (재부팅 시 복원) |
| 중복 패킷 dedup | LRU 64-슬롯 캐시로 30초 이내 중복 패킷 무시 (메시 루프 방지) |
| TTL 감소 | 중계 시마다 TTL 1 감소, TTL=0이면 더 이상 전달 안 함 (최대 7홉) |
| Serial UART 로그 | 115200bps로 사람이 읽기 쉬운 텍스트 + 패킷 hex dump 출력 |
| **배터리+절전 모드** | **ESP-PM 라이트 슬립 + BLE 저듀티 (1.28s 광고/스캔) + LED 펄스 + 딥슬립 (GPIO 0 wake)** |
| 다중 빌드 지원 | PlatformIO + ESP-IDF + Arduino IDE 3개 환경 모두 빌드 가능 |

---

## 전원 모드

두 가지 전원 모드를 컴파일 타임에 선택할 수 있습니다. **배터리+절전이 기본값**입니다.

### 배터리+절전 (기본, `BC_POWER_BATTERY=1`)

| 항목 | 설정값 | 효과 |
|------|--------|------|
| BLE advertising 간격 | 1.28~2.05s | 발견 속도 ↓, 전력 ↓ |
| BLE scan 윈도우 | 30ms / 1.28s 주기 (2.3% 듀티) | 거의 항상 대기, peer 발견 지연 |
| BLE scan burst 주기 | 2초 on / 28초 off | 30초당 2초만 활발히 스캔 |
| CPU 클럭 | 80~240MHz DFS (자동) | 활동 시 240MHz, idle 시 80MHz |
| CPU 슬립 | 라이트 슬립 (BLE 컨트롤러 유지) | 패킷 수신 시 마이크로초 단위 wake |
| 상태 LED | 1초당 50ms on (5% 듀티) | 살아있음 표시 |
| 배터리 전압 모니터링 | 1분마다 ADC 측정 | 저전압 경고 / 중지 |
| 딥슬립 진입 | 5분간 무활동 시 | 60초 타이머 or GPIO 0 버튼으로 wake |

### USB 상시 전원 (`BC_POWER_USB_ALWAYS=1`)

| 항목 | 설정값 |
|------|--------|
| BLE advertising 간격 | 100ms (빠른 발견) |
| BLE scan 윈도우 | 40ms / 80ms 주기 (50% 듀티) |
| BLE scan burst | 5초 연속 / 0.5초 간격 (거의 항상) |
| CPU 클럭 | 240MHz 고정 |
| 슬립 | 없음 (24/7 풀 가동) |
| LED/배터리 모니터링 | 비활성 |

### 예상 전력 소비 & 동작 시간

| 모드 | 보드 | 평균 전류 | 18650 3000mAh 기준 동작 시간 |
|------|------|-----------|------------------------------|
| 라이트 슬립 (BLE on) | ESP32 클래식 | 8-12 mA | 250-375 시간 (10-15일) |
| 라이트 슬립 (BLE on) | ESP32-C3 | 4-6 mA | 500-750 시간 (20-31일) |
| 딥슬립 (5분 무활동) | ESP32 클래식 | 0.18 mA | 16,000 시간 (약 2년) |
| 딥슬립 (5분 무활동) | ESP32-C3 | 0.005 mA (RTC) | ~70,000 시간 (약 8년) |
| USB 상시 풀 가동 | ESP32 클래식 | 60-80 mA | 37-50 시간 (1.5-2일) |
| USB 상시 풀 가동 | ESP32-C3 | 35-50 mA | 60-85 시간 (2.5-3.5일) |

> **참고**: 실제 동작 시간은 BLE 연결 수, 패킷 빈도, 온도, 배터리 상태에 따라 달라집니다.
> 위 수치는 BLE 연결 1-2개 + 패킷 1개/분 수준의 일반적 메시 노드 기준입니다.

### 하드웨어 연결 (배터리 모드)

```
ESP32 클래식 (DevKitC)        배터리 (LiPo 3.7V)
├── GPIO 2   ─── LED (온보드, 5% 듀티 펄스)
├── GPIO 0   ─── BOOT 버튼 (딥슬립 wake용, 이미 보드에 있음)
├── GPIO 35  ─── 배터리 (+) → 100kΩ → ADC → 100kΩ → GND
│                (분배비 2:1, LiPo 4.2V → ADC 2.1V, 안전 측정 가능)
├── 3.3V     ─── MCP1700 LDO → 배터리 (+) (3.7V → 3.3V 변환)
└── GND      ─── 배터리 (-)

ESP32-C3 (DevKitM-1)
├── GPIO 8   ─── LED (온보드)
├── GPIO 9   ─── BOOT 버튼 (딥슬립 wake)
├── GPIO 4   ─── 배터리 ADC (분배 2:1)
└── 3.3V     ─── LDO → 배터리 (+)
```

> C3의 배터리 ADC 핀은 `BC_BATTERY_ADC_PIN` 매크로를 수정하여 변경할 수 있습니다.

### 빌드 환경별 모드 선택

PlatformIO에서는 env 이름으로 선택:

```bash
# 배터리 모드 (기본)
pio run -e esp32dev       # ESP32 클래식 + 배터리
pio run -e esp32c3        # ESP32-C3 + 배터리

# USB 상시 전원 모드 (이전 동작)
pio run -e esp32dev-usb   # ESP32 클래식 + USB
pio run -e esp32c3-usb    # ESP32-C3 + USB
```

ESP-IDF에서는 `idf.py menuconfig`에서 `CONFIG_PM_ENABLE`를 켜면 자동으로
배터리 모드 활성화. `-DBC_POWER_BATTERY=1`은 `main/CMakeLists.txt`에
추가하거나 `set-target` 후 sdkconfig에서 직접 활성화 가능합니다.

Arduino IDE에서는 build flags에 `-DBC_POWER_BATTERY=1`을 추가 (또는 생략 시
소스 코드의 `#ifndef` 분기에 의해 자동으로 배터리 모드가 됨).

---

## 하드웨어 요구사항

- **지원 보드**: ESP32 클래식 (ESP-WROOM-32, DevKitC, NodeMCU-32S 등)
  또는 ESP32-C3 (DevKitM-1, Seeed XIAO ESP32C3, ESP32-C3-DevKitC-02 등)
- **플래시**: 4MB 이상 (기본 파티션 테이블 사용)
- **전원**: USB 5V 상시 전원 (배터리 절전 모드 미구현)
- **추가 모듈**: 불필요 (BLE 온칩으로 충분)
- **시리얼**: USB-Serial 브릿지 (CP2102/CH340 또는 C3 내장 USB CDC)가 보드에 내장

### 핀 연결

별도 GPIO 핀 사용 없음. USB Serial만 사용:
- TX/RX는 보드에 내장된 USB-Serial 브릿지를 통해 PC와 연결
- BLE 안테나는 ESP32 모듈에 PCB 내장

---

## 패킷 프로토콜 구조

bitchat 호환 패킷 포맷 (빅엔디언):

```
+------+------+------+------+----------------------+
|magic | ver  | type | ttl | sender_pk (32 bytes)  |
| 0xBC | 0x01 | 1B   | 1B | (Ed25519 공개키)        |
+------+------+------+------+----------------------+
| counter (8 bytes, msb first)                     |
+--------------------------------------------------+
| payload_len (2 bytes, big endian)               |
+--------------------------------------------------+
| payload (0..512 bytes, 암호화된 메시지/메타)       |
+--------------------------------------------------+
| signature (64 bytes, Ed25519 over header+payload)|
+--------------------------------------------------+
```

**패킷 타입** (1바이트 `type` 필드):

| 값 | 이름 | 용도 |
|----|------|------|
| 0x01 | CHAT | 암호화된 채팅 메시지 (ECDH + AES-GCM) |
| 0x02 | ACK | 수신 확인 |
| 0x03 | PEER_INFO | 피어 발견 알림 (닉네임/공개키) |
| 0x04 | PEER_REQUEST | 피어 목록 요청 |
| 0x05 | LEAVE | 메시 이탈 |
| 0x06 | KEY_EXCHANGE | ECDH 공개키 교환 |
| 0x07 | PING | keep-alive |

**검증 규칙**:
1. `magic == 0xBC` and `version == 0x01`
2. `ttl <= 7` (무한 루프 방지)
3. `payload_len <= 512`
4. Ed25519 서명이 `sender_pk` 기준 유효
5. (선택) LRU dedup 캐시로 30초 이내 중복 무시

---

## 빌드 환경별 가이드

### 1) PlatformIO (권장)

가장 빠르고 의존성 관리가 자동입니다. ESP32 클래식과 ESP32-C3 모두 지원.

```bash
# PlatformIO CLI 설치 (필요시)
pip install platformio

# ESP32 클래식 빌드
cd bitchat-esp32-relay
pio run -e esp32dev
pio run -t upload -e esp32dev
pio device monitor -e esp32dev

# ESP32-C3 빌드
pio run -e esp32c3
pio run -t upload -e esp32c3
pio device monitor -e esp32c3
```

**VS Code PlatformIO 확장**을 쓰면 하단 바에서 Build/Upload/Monitor 버튼을
클릭만 하면 됩니다. 환경 선택은 하단 바의 env 선택기(`esp32dev` 또는 `esp32c3`)를
클릭하여 전환합니다.

### 2) ESP-IDF (네이티브)

ESP-IDF v5.x 이상 필요. arduino-esp32를 컴포넌트로 추가해야 합니다.

```bash
# ESP-IDF 환경 설정 (최초 1회)
export IDF_PATH=/path/to/esp-idf
. $IDF_PATH/export.sh

# arduino-esp32 컴포넌트 추가
cd bitchat-esp32-relay/components
git clone --depth 1 --recurse-submodules \
    https://github.com/espressif/arduino-esp32.git arduino
cd ..

# ===== ESP32 클래식 빌드 =====
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor

# ===== ESP32-C3 빌드 (C3 전용 sdkconfig.defaults 병합됨) =====
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
# (C3는 /dev/ttyACM0 또는 /dev/ttyUSB0 - 보드에 따라 다름)
```

`sdkconfig.defaults`가 공통 설정을, `sdkconfig.defaults.esp32c3`가
C3 전용 오버라이드(BLE 동시 연결 4, FreeRTOS unicore, 메모리 절약)를
담당합니다. `idf.py set-target`이 자동으로 적합한 defaults를 병합합니다.

### 3) Arduino IDE

소스가 `src/` 하위에 있으므로, Arduino IDE용 스케치 폴더를 먼저 생성합니다.

```bash
cd bitchat-esp32-relay
bash setup_arduino_ide.sh
```

이 스크립트는 `arduino_ide/bitchat_relay/` 폴더를 만들고 모든 소스 파일을 복사합니다.

그 다음:

1. Arduino IDE를 열고 `arduino_ide/bitchat_relay/bitchat_relay.ino`를 엽니다.
2. **Tools > Board** 메뉴에서 보드 선택:
   - ESP32 클래식: `ESP32 Dev Module`
   - ESP32-C3: `ESP32C3 Dev Module` (또는 `Seeed XIAO ESP32C3`)
3. **Tools > Upload Speed** = `921600` (C3 USB CDC 보드는 `115200`로 낮출 것)
4. **Tools > Port** = `/dev/ttyUSB0` (Linux) 또는 `COM3` (Windows)
5. **Upload** 버튼 클릭
6. **Tools > Serial Monitor** 열기, baud를 115200으로 설정

> **참고**: Arduino IDE는 `setup()`/`loop()`가 있는 `.ino` 파일을 자동으로
> 진입점으로 사용하며 같은 폴더의 `.cpp` 파일들을 모두 같이 컴파일합니다.
> setup_arduino_ide.sh가 자동으로 `main.cpp`를 `.ino`로 복사하고 원본을
> 지워 중복 정의를 방지합니다.
>
> **ESP32-C3 한계**: C3는 SRAM이 400KB로 ESP32 클래식(520KB)보다 작아
> 동시 BLE 연결 수가 4개로 제한됩니다. 펌웨어는 자동으로 매크로
> (`BOARD_VARIANT_ESP32C3`)를 통해 연결 수와 큐 크기를 축소합니다.

---

## 시리얼 로그 예시

부팅 후 다음과 같은 로그가 115200bps로 출력됩니다:

```
=============================
 bitchat ESP32 relay firmware
 build: Oct 27 2026 16:23:45
=============================

[I][bitchat] crypto: DRBG ready
[I][bitchat] no self seed in NVS, generating new keypair...
[I][bitchat] self keypair persisted
[I][bitchat] === storage dump ===
[I][bitchat] self_id=8a3f2c1b9e04 ready=1 counter=0 peers=0
[I][bitchat] BLE advertising started as bchat-relay
[I][bitchat] === bitchat relay node ready ===

[D][bitchat] found bitchat peer: aa:bb:cc:dd:ee:ff rssi=-42
[I][bitchat] connecting to peer aa:bb:cc:dd:ee:ff ...
[I][bitchat] connected to peer aa:bb:cc:dd:ee:ff (out=1)

[D][bitchat] [RX] len=120
0000  bc 01 01 03 8a 3f 2c 1b 9e 04 5d a2 f3 88 12 c6  |.....?,.....]....|
0010  44 21 ee 67 ab 99 6c 11 e3 c4 88 0f a2 56 33 02  |D!.g..l......V3.|
0020  9d 18 4c 22 e1 7f 00 00 00 00 00 00 00 00 02 00  |..L"............|
0030  48 65 6c 6c 6f 20 66 72 6f 6d 20 6e 6f 64 65 21  |Hello from node!|
...
[I][bitchat] signature OK from sender=8a3f2c1b9e04
[I][bitchat] forwarded pkt (ttl 7->6) to other peers
```

---

## 파일 구조

```
bitchat-esp32-relay/
├── README.md                      # 이 파일
├── platformio.ini                 # PlatformIO 빌드 (4 env: esp32dev/c3 + USB/battery)
├── CMakeLists.txt                 # ESP-IDF 루트
├── sdkconfig.defaults             # ESP-IDF 기본 (ESP32 공통 + PM/tickless)
├── sdkconfig.defaults.esp32c3     # ESP-IDF C3 전용 (BLE 4conn, unicore, PM)
├── bitchat_relay.ino              # Arduino IDE 진입 sketch (루트)
├── setup_arduino_ide.sh           # Arduino IDE 폴더 자동 생성 스크립트
├── src/                           # 표준 소스 폴더
│   ├── main.cpp                   # Arduino 진입 (setup/loop) + power 연동
│   ├── log.h                      # Serial 로깅 매크로
│   ├── bitchat_protocol.h/.cpp    # 패킷 직렬화/검증
│   ├── crypto.h/.cpp              # Ed25519 (mbedtls)
│   ├── storage.h/.cpp             # NVS 키/피어 영속화
│   ├── power.h/.cpp               # ESP-PM 라이트 슬립 + 배터리 ADC + LED 펄스
│   ├── lora_relay.h/.cpp          # LoRa 자동감지 + BLE↔LoRa 브릿지 (SX127x)
│   ├── retry_queue.h/.cpp        # ACK 재전송 (패시브 ACK + 타임아웃)
│   ├── blacklist.h/.cpp          # pk/BLE 주소 블랙리스트 (NVS 영속화)
│   └── ble_relay.h/.cpp           # BLE GATT + relay (배터리 모드 시 저듀티)
├── lib/NimBLE-Arduino/            # h2zero NimBLE-Arduino 1.4.2 라이브러리 (자체 포함)
├── lib/LoRa/                      # Sandeep Mistry arduino-LoRa 0.8.0 (SX127x 자동 감지)
├── main/
│   └── CMakeLists.txt             # ESP-IDF main 컴포넌트 (../src/ 참조)
└── components/                    # ESP-IDF용 외부 컴포넌트 (arduino-esp32)
```

---

## 동작 원리 요약

1. **부팅 시**: NVS에서 자체 Ed25519 키쌍을 복원. 없으면 새로 생성 후 저장.
2. **BLE 서버**: `0000bc00-...` 서비스로 advertise. 클라이언트 write용 RX char,
   notify용 TX char 제공.
3. **BLE 클라이언트**: 동시에 scanner를 돌려 같은 서비스를 advertise하는
   주변 노드 발견. 자리가 남으면 자동으로 연결 + notify 구독.
4. **패킷 수신**:
   - 클라이언트가 우리 서버에 write하거나,
   - 우리가 연결한 peer가 notify로 push하거나.
5. **검증 단계**:
   - magic/version/TTL/payload_len 검증
   - signing message 계산 (magic..payload 직렬화)
   - mbedtls Ed25519 verify against sender_pk
6. **중계**:
   - dedup 캐시 확인 (중복이면 drop)
   - peer 목록에 sender_pk 추가
   - TTL 1 감소 (TTL=0이면 더 이상 forward 안 함)
   - 다른 모든 연결된 peer에게 notify/write로 broadcast

---

## ACK 재전송 로직 (retry_queue)

`src/retry_queue.h/.cpp`에 구현된 메시 프로토콜의 핵심 신뢰성 메커니즘.

### 작동 흐름

```
                   패킷 수신
                       │
                       ▼
            ┌──────────────────────┐
            │ 1. 패킷 파싱 + 검증   │
            └──────────────────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │ 2. passive ACK 체크    │  ← 우리가 forward한 패킷이
            │    (retry_queue에서)   │    다른 peer에 의해 재송신되면
            │                         │    우리 forward 성공으로 간주
            └──────────────────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │ 3. Ed25519 서명 검증   │
            └──────────────────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │ 4. dedup 캐시 체크     │  ← 30초 이내 중복 drop
            └──────────────────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │ 5. TTL 감소 + forward  │
            └──────────────────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │ 6. retry_queue에 추가  │  ← pending entry 생성
            │    (타임아웃 추적용)    │
            └──────────────────────┘
                       │
                       ▼
        ┌──────────────────────────────────┐
        │ 주기적 체크 (1초마다, loop)        │
        ├──────────────────────────────────┤
        │ 2초 경과 + retry<3 → 재전송     │
        │ retry>=3 → drop                  │
        └──────────────────────────────────┘
```

### 두 가지 ACK 메커니즘

**1) 패시브 ACK (Passive ACKnowledgment)**

메시 프로토콜의 핵심 패턴. 우리가 forward한 패킷이 다른 peer에 의해 다시 브로드캐스트되는 것을 감지하면, 우리 forward가 성공했다고 간주하고 재전송 큐에서 제거.

- BLE mesh, Thread, Zigbee 등에서 사용하는 패턴
- 별도의 ACK 패킷 필요 없음 → 네트워크 트래픽 절감
- 단점: 한 홉의 성공만 보장 (최종 목적지 도달은 별도 확인)

**2) 명시적 ACK (Explicit ACK, type=0x02)**

bitchat ACK 패킷의 payload에서 원본 (sender_pk, counter)를 추출하여 retry_queue에서 매칭. 최종 목적지 도달을 의미.

- ACK 패킷 형식: `sender_pk(32B) || counter(8B) || ack_data(옵션)`
- relay 노드는 ACK를 다른 peer에게 forward (원 송신자에게 도달하도록)
- relay 자체의 pending entry도 함께 정리

### 설정값 (`src/retry_queue.h`)

| 매크로 | 기본값 | 의미 |
|--------|--------|------|
| `BC_RETRY_QUEUE_SIZE` | 16 | 동시 추적할 pending entry 수 (16 × 623B = 10KB RAM) |
| `BC_RETRY_TIMEOUT_MS` | 2000 | 단일 홉 ACK 대기 시간 (2초) |
| `BC_RETRY_MAX_RETRIES` | 3 | 최대 재시도 횟수 (총 4회 송신) |
| `BC_RETRY_HASH_LEN` | 16 | sender_pk 앞 8B + counter 8B (패킷 식별용) |

### 통계 출력

5초마다 Serial에 출력되는 상태:

```
[I][bitchat] --- retry: pending=2 retrans=5 p_ack=18 e_ack=3 dropped=1 ---
```

- `pending`: 현재 ACK 대기 중인 패킷 수
- `retrans`: 타임아웃으로 재전송한 횟수
- `p_ack`: 패시브 ACK로 제거된 entry 수 (forward 성공)
- `e_ack`: 명시적 ACK로 제거된 entry 수
- `dropped`: 최대 재시도 초과로 drop된 패킷 수

### 한계 및 향후 개선

- **큐 크기 16**: 메시 트래픽이 많을 시 가득 차서 가장 오래된 entry 강제 제거 (FIFO)
- **2초 타임아웃**: BLE mesh 일반적이지만, 다홉 메시 노드 거치면 총 18초 지연 가능
- **RAM 사용량**: C3에서 +3.2% (10KB) 증가. 필요시 `BC_RETRY_QUEUE_SIZE` 축소 가능 (8로 변경 시 5KB 절약)
- **최종 목적지 도달 보장**: 패시브 ACK는 한 홉만 보장. 종단 간 신뢰성은 송신자가 별도 처리 필요


---

## 블랙리스트 필터링 (blacklist)

`src/blacklist.h/.cpp`에 구현된 두 가지 차단 메커니즘. NVS에 영속화되어 재부팅 후에도 유지.

### 두 가지 블랙리스트

**1) Ed25519 공개키 블랙리스트 (32B × 32개 슬롯)**

- bitchat 노드의 정식 식별자(Ed25519 공개키)로 차단
- 차단된 송신자의 패킷은 서명 검증 **전에** drop → crypto 연산 절약
- 32B × 32 = 1KB NVS blob

**2) BLE 주소 블랙리스트 (6B × 16개 슬롯)**

- 낮은 수준의 차단 - **연결 시도 자체를 안 함**
- advertising 단계에서 필터링하여 BLE 자원 절약
- 6B × 16 = 96B NVS blob

### Serial 명령 인터페이스 (실시간 관리)

USB Serial Monitor에서 입력 가능한 명령:

```text
> help                        # 도움말 출력
> bl                          # 현재 블랙리스트 dump
> bl stats                    # 카운트만 출력
> bl on                       # 블랙리스트 활성화 (기본)
> bl off                      # 블랙리스트 비활성화 (모든 체크 skip)
> bl add 8a3f2c1b9e04...      # Ed25519 pk 추가 (32B = 64 hex chars)
> bl add aabbccddeeff          # BLE 주소 추가 (6B = 12 hex chars)
> bl rm 8a3f2c1b9e04...       # 제거
> bl clear                    # 전체 초기화
```

> 길이에 따라 pk(32B)인지 BLE(6B)인지 자동 감지합니다.

### 작동 위치

```
패킷 수신
    │
    ▼
1) 패킷 파싱 + magic/TTL 검증
    │
    ▼
2) ★ 블랙리스트 체크 (sender_pk)  ← 차단 시 drop (서명 검증 안 함)
    │
    ▼
3) ACK 재전송 passive/explicit ACK 처리
    │
    ▼
4) Ed25519 서명 검증
    │
    ▼
5) dedup 캐시 체크
    │
    ▼
6) TTL 감소 + forward

BLE advertising 수신:
    │
    ▼
1) 서비스 UUID 매칭
    │
    ▼
2) ★ 블랙리스트 체크 (BLE 주소)  ← 차단 시 연결 안 함
    │
    ▼
3) 자동 연결 + notify 구독
```

### Serial 로그 예시

```
[I][bitchat] blacklist: initialized (enabled=1, pk=2, ble=1)
[I][bitchat] blacklist: added pk id=8a3f2c1b9e04 (slot 0, total=1)
[W][bitchat] blacklist: dropping packet from blocked sender=8a3f2c1b9e04
[D][bitchat] blacklist: skipping blocked ble peer aa:bb:cc:dd:ee:ff

> bl stats
[I][bitchat] blacklist stats: pk=2 ble=1 enabled=1

> bl off
[I][bitchat] blacklist: DISABLED
```

### 설정값 (`src/blacklist.h`)

| 매크로 | 기본값 | 의미 |
|--------|--------|------|
| `BC_BL_MAX_PK_ENTRIES` | 32 | Ed25519 공개키 최대 저장 수 (1KB NVS) |
| `BC_BL_MAX_BLE_ENTRIES` | 16 | BLE 주소 최대 저장 수 (96B NVS) |

### 보안 고려사항

- Serial 명령은 USB 연결된 사용자만 실행 가능 → 물리적 접근 필요
- 운용 환경에서 USB를 분리하면 블랙리스트 변경 불가 (재부팅 후에도 NVS 유지)
- NVS는 암호화되지 않음 → 공격자가 물리적 접근 시 변조 가능
- 고신뢰 환경: ESP-IDF NVS Encryption 기능 (`CONFIG_NVS_ENCRYPTION=y`) 활성화 권장


---

## NimBLE BLE 스텍 (Bluedroid 대안)

본 펌웨어는 **NimBLE-Arduino** (h2zero) 라이브러리를 사용하여 ESP-IDF의 기본 Bluedroid 대신 더 가벼운 NimBLE 스텍을 사용합니다.

### NimBLE vs Bluedroid 장점

| 항목 | Bluedroid | NimBLE | 효과 |
|------|-----------|--------|------|
| 코드 크기 (Flash) | 큼 (≈500KB+ libs) | 작음 (≈250KB) | Flash 30-40% 절약 |
| RAM 사용량 | 커넥션당 5-7KB | 커넥션당 1-2KB | 동일 RAM으로 2-3배 많은 연결 |
| C3 동시 연결 | 4 (SRAM 제약) | 8 (heap 여유) | +4 연결 |
| S3 동시 연결 | 7 | 12 | +5 연결 |
| Classic 동시 연결 | 7 | 12 | +5 연결 |
| BLE 5.0 | 미지원 (4.2 클래식) | 지원 | Extended adv, LE Privacy 등 |

### 빌드 크기 비교 (배터리 모드)

| 보드 | Bluedroid | NimBLE | Flash 변화 |
|------|-----------|--------|------------|
| ESP32 클래식 | 96.6% (1.27MB) | 58.1% (761KB) | **-510KB ↓** (huge_app partition 불필요) |
| ESP32-C3 | 86.3% (1.13MB) | 52.3% (685KB) | **-476KB ↓** |
| ESP32-S3 | 81.7% (1.07MB) | 52.7% (690KB) | **-384KB ↓** |

Flash 여유가 50%로 늘어나 향후 WiFi/MQTT 브릿지, OLED 디스플레이, OTA 업데이트 등 추가 기능을 넣을 수 있는 여유가 큼.

### API 차이 (마이그레이션 참고사항)

| Bluedroid API | NimBLE API | 비고 |
|---------------|-----------|------|
| `BLEDevice::init()` | `NimBLEDevice::init()` | 동일 |
| `BLEServer*` | `NimBLEServer*` | 동일한 패턴 |
| `BLE2902` (CCCD) | 명시적 생성 불필요 | `PROPERTY::NOTIFY` 설정만으로 자동 |
| `BLEAdvertisedDeviceCallbacks::onResult(dev)` | `onResult(NimBLEAdvertisedDevice* dev)` | pointer로 전달 |
| `BLEAddress::getNative()` (esp_bd_addr_t*) | `NimBLEAddress::getNative()` (const uint8_t*) | deref 불필요 |
| `BLEScan::setInterval(0x800)` (0.625ms 단위) | `NimBLEScan::setInterval(1280)` (ms 단위) | 단위 다름 |
| `BLECharacteristicCallbacks::onWrite(ch)` | `NimBLECharacteristicCallbacks::onWrite(ch)` | NimBLE 1.4.2는 단일 인자 |
| `BLEClient* delete client` | `NimBLEDevice::deleteClient(c)` | 소멸자 private |
| `registerForNotify(callback)` | `subscribe(true, callback)` | API 변경 |
| `LOG_LEVEL_ERROR` (사용자 정의) | `BC_LOG_LEVEL_ERROR` | NimBLE 헤더와 충돌 회피 |

### 컴파일 설정

`sdkconfig.defaults`에서 Bluedroid 비활성 + BT 컨트롤러만 활성:

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=n        # NimBLE-Arduino 자체 구현 사용
CONFIG_BT_BLUEDROID_ENABLED=n     # Bluedroid 비활성 (60KB+ RAM 절약)
CONFIG_BTDM_CONTROLLER_MODE_BTDM=y
```

`lib/NimBLE-Arduino/` 디렉토리에 라이브러리 소스가 포함되어 있어 별도 설치 불필요.

### 한계

- **BLE 4.2 클래식 보드 (ESP32)**: NimBLE는 BLE 5.0 일부 기능 (extended advertising, LE Privacy)을 사용하지만, 클래식 하드웨어 한계로 4.2까지만 지원
- **MTU 247B** (NimBLE 기본) vs Bluedroid 517B - 큰 패킷은 BLE fragmentation 필요
- **NimBLE 1.4.2 API 고정**: 최신 NimBLE-Arduino 2.x와 API 다름 (v3.x arduino-esp32 필요)


---

## LoRa 모듈 자동 감지 + BLE↔LoRa 브릿지

본 펌웨어는 **arduino-LoRa** (Sandeep Mistry) 라이브러리를 사용하여 SX127x 시리즈 LoRa 모듈을 자동 감지합니다. 모듈이 발견되면 BLE↔LoRa 브릿지가 자동으로 활성화되어, BLE 패킷이 LoRa로도 브로드캐스트되고 LoRa에서 들어온 패킷이 BLE 메시로 전달됩니다.

### 자동 감지 핀아웃 후보

`src/lora_relay.cpp`에서 다음 핀아웃을 순차 시도합니다:

| 후보 | SCK | MISO | MOSI | CS | RST | IRQ | 호환 보드 |
|------|-----|------|------|----|----|------|-----------|
| 1 | 5 | 19 | 27 | 18 | 14 | 26 | Heltec WiFi LoRa 32 V1/V2, TTGO LoRa32 V1 |
| 2 | 5 | 19 | 27 | 18 | 23 | 26 | TTGO T-Beam |
| 3 | 18 | 19 | 23 | 5 | 14 | 26 | Dragino Lora32 / Generic |
| 4 | 18 | 19 | 23 | 5 | 14 | 33 | Alternative CS/RST/IRQ |

각 후보에 대해:
1. SPI init (SCK, MISO, MOSI, CS)
2. LoRa.setPins(cs, rst, irq)
3. LoRa.begin(freq) → 1이면 OK
4. RegVersion (0x42) 읽기 → 0x12이면 SX1276 정상 인식

### LoRa 설정 (기본값)

| 항목 | 기본값 | 설명 |
|------|--------|------|
| Frequency | 921 MHz | 한국 KR 대역 (920-923MHz) |
| Bandwidth | 125 kHz | LoRa.setSignalBandwidth(125000) |
| Spreading Factor | 7 | LoRa.setSpreadingFactor(7) - 빠른 전송 |
| Coding Rate | 4/5 | LoRa.setCodingRate4(5) |
| TX Power | 17 dBm | LoRa.setTxPower(17) - SX1276 max |
| Sync Word | 0x34 | 커스텀 sync word (bitchat-LoRa 구분) |
| Preamble Length | 8 symbols | LoRa.setPreambleLength(8) |
| CRC | enabled | LoRa.enableCrc() |

### 동작 흐름

```
[BLE 패킷 수신]                     [LoRa 패킷 수신]
       │                                    │
       ▼                                    ▼
process_packet                       onReceive callback
       │                                    │
       ├──── forward_to_others ─────►      │
       │     ├─ LoRa TX (broadcast)        │
       │     ├─ BLE notify (incoming)      │
       │     └─ BLE write (outgoing)      │
       │                                    │
       └────────────────────────────────► enqueue_packet (BLE 큐)
                                                  │
                                                  ▼
                                          process_packet
                                          (BLE와 동일 처리)
```

- LoRa에서 수신된 패킷은 BLE relay 큐에 enqueue → 동일하게 서명 검증 + dedup + forward
- BLE에서 들어온 패킷은 LoRa로도 브로드캐스트 (lora_relay_broadcast)
- LoRa → BLE → BLE → LoRa 등 다중 홉 브리지 자동 동작

### 한계

- **SX127x만 지원**: SX1262/1268 (Heltec V3 등)은 다른 라이브러리 (RadioLib) 필요
- **단일 주파수 채널**: FHSS 미구현, 동일 채널의 다른 LoRa 트래픽과 충돌 가능
- **큰 패킷 drop**: LoRa 프레임 최대 222B. bitchat 패킷(47B header + payload + 64B sig)이 222B 초과 시 drop
  - 작은 패킷 (ACK, PING, PEER_INFO 등)은 잘 동작
  - 큰 CHAT 메시지는 BLE 전용 전달, LoRa로는 drop + 로그 경고
- **LoRa 속도**: SF7, BW125 → ~5800 bps (BLE 1Mbps와 비교하면 1/170 느림)
- **듀티 사이클**: 한국 920MHz 대역은 5mW 이하 출력 제한 → 17dBm 사용 시 규제 확인 필요

### 하드웨어 연결 (대표 보드)

**Heltec WiFi LoRa 32 (V2)** - 가장 일반적, ESP32 + SX1276 내장:
```
ESP32 GPIO 5  ─── SX1276 SCK
ESP32 GPIO 19 ─── SX1276 MISO
ESP32 GPIO 27 ─── SX1276 MOSI
ESP32 GPIO 18 ─── SX1276 NSS (CS)
ESP32 GPIO 14 ─── SX1276 RST
ESP32 GPIO 26 ─── SX1276 DIO0 (IRQ)
3.3V           ─── SX1276 VCC
GND            ─── SX1276 GND
```

**TTGO T-Beam** (배터리 + GPS 내장):
- RST만 GPIO 14 → 23으로 변경, 나머지 동일

**독립형 SX1276 모듈** (RFM95W, SX1278 보드 등):
- ESP32 SPI 기본 핀 사용: SCK=18, MISO=19, MOSI=23
- CS, RST, IRQ는 임의 GPIO (5, 14, 26 추천)

### Serial 로그 예시

```
[I][bitchat] lora: scanning for SX127x module (4 pinout candidates)...
[D][bitchat] lora: trying Heltec/TTGO LoRa32 V1/V2 (sck=5 miso=19 ...)
[I][bitchat] lora: ✓ Heltec/TTGO LoRa32 V1/V2 detected (SX1276, version=0x12)
[I][bitchat] lora: freq=921.0MHz bw=125kHz SF=7 CR=4/5 pwr=17dBm sync=0x34
[I][bitchat] LoRa bridge ENABLED: Heltec/TTGO LoRa32 V1/V2

[I][bitchat] --- lora dump ---
[I][bitchat] module:  Heltec/TTGO LoRa32 V1/V2
[I][bitchat] freq:    921.0 MHz
[I][bitchat] stats:   rx=18 tx=12 drop_large=0 drop_parse=0
```

LoRa 모듈이 없으면 자동으로 BLE-only 모드로 동작합니다:
```
[I][bitchat] LoRa module not detected - BLE-only mode
```

### 빌드 결과 (LoRa 자동감지 + BLE↔LoRa 브릿지 추가)

| 보드 | RAM | Flash | 비고 |
|------|-----|-------|------|
| ESP32 클래식 | 16.3% (53KB) | 59.2% (776KB) | LoRa 미탑재시 자동 BLE-only |
| ESP32-C3 | 12.8% (42KB) | 53.4% (700KB) | Heltec V3 (SX1262) 미지원 |
| ESP32-S3 | 14.8% (48KB) | 53.8% (705KB) | Heltec Wireless Stick 등 |

Flash 사용량이 ~15KB 증가했지만 여전히 50% 미만으로 쾌적.

### 빌드 환경

`lib/LoRa/` 디렉토리에 arduino-LoRa (Sandeep Mistry, 28KB 소스)가 포함되어 있어 별도 설치 불필요. PlatformIO가 자동으로 인식합니다.

```ini
# platformio.ini (이미 설정됨)
[env]
framework = arduino
# lib/LoRa, lib/NimBLE-Arduino 자동 인식
```


---

## BLE 동시 연결 한계 분석

### ESP32 BLE 컨트롤러 하드웨어 한계

| 보드 | BLE 컨트롤러 최대 연결 | 비고 |
|------|----------------------|------|
| ESP32 클래식 | **9개** (silicon limit) | 칩 자체 한계, 소프트웨어로 못 바꿈 |
| ESP32-C3 | **9개** | 동일 |
| ESP32-S3 | **9개** | 동일 |

ESP32 시리즈 BLE 컨트롤러는 칩 자체가 9개까지만 accept합니다. NimBLE 설정을 12, 16으로 올려도 버퍼만 낭비하고 실제로는 9개까지만 연결됩니다.

### NimBLE 설정 vs 실제 한계

| 설정값 | NimBLE 버퍼 할당 | 실제 동작 연결 수 | 메모리 낭비 |
|--------|-------------------|-------------------|-------------|
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3` (기본) | 3개분 | 3개 | 없음 |
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9` | 9개분 | **9개** (최대) | 없음 ✓ |
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=12` | 12개분 | 9개 (컨트롤러 한계) | +1KB 버퍼 낭비 |

### 본 펌웨어의 안전 설정

**앱 레벨 8 + NimBLE 9** = 안전 마진 1개:

```c
// lib/NimBLE-Arduino/src/nimconfig.h
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 9    // 컨트롤러 한계 = NimBLE 한계

// src/ble_relay.h
#define BC_MAX_OUT_CONN  4    // outgoing (클라이언트로 연결)
#define BC_MAX_IN_CONN   4    // incoming (서버로 받음)
// 총 8 + NimBLE 버퍼 1개 headroom = 9 (컨트롤러 한계 일치)
```

### 왜 9(최대) 대신 8(안전)인가?

| 설정 | 장점 | 단점 |
|------|------|------|
| 9 (최대) | 동시 연결 1개 더 | NimBLE saturation 시 heap spike 위험 |
| **8 (안전)** ✅ | NimBLE 1개 headroom, runtime 안정성 ↑ | 1개 적음 (보통 문제 안 됨) |
| 7 (보수적) | 큰 안전 마진 | 2개 손실, 메시 허브용 부족할 수 있음 |

안전 마진이 필요한 이유:
1. NimBLE가 9개 연결 모두 처리 중 transient heap peak 발생 가능
2. BLE 컨트롤러 saturation 시 advertising/scan 성능 저하
3. Production stability를 위한 엔지니어링 관행 (80-90% 수준)
4. 9번째 연결이 갑자기 들어와도 reject되지 않고 buffer 할당 가능

### 실제 연결 수가 부족할 때 해결책

ESP32 칩 하나당 9개가 절대 한계이므로, **10+ 연결이 필요하면 다중 ESP32 노드로 분산**해야 합니다:

```
[스마트폰들 5대]
       │
   [ESP32-A] ← max 9 conn
       │ (BLE 메시 간 forwarding)
   [ESP32-B] ← max 9 conn
       │
   [ESP32-C] ← max 9 conn
```

- 각 ESP32 노드가 9개씩 처리 → 3개 노드로 27개 연결까지 확장
- 노드 간은 BLE 메시로 forwarding (본 펌웨어가 자동 처리)
- linear chain으로 배포하면 trail/path 따라 무한 확장 가능

### 실측 가능한 연결 수 시나리오

| 시나리오 | 필요 conn 수 | 단일 C3 (8 conn) | 다중 노드 필요? |
|----------|-------------|------------------|------------------|
| 가정용 소규모 그룹 (4명) | 4 | ✅ 충분 | ❌ |
| 소규모 회의 (8명) | 8 | ✅ 충분 (max) | ❌ |
| 중규모 회의 (15명) | 15 | ❌ 부족 | ✅ 2대 필요 |
| 대규모 이벤트 (50명+) | 50+ | ❌ | ✅ 6+대 필요 |
| 밀집 메시 (5+ ESP32 노드) | 5-8 | ✅ 가능 | ❌ |


---

## 제한사항

- **ESP32 BLE 동시 연결**:
  - ESP32 클래식: 최대 7개 (incoming 4 + outgoing 3)
  - ESP32-C3: 최대 4개 (incoming 2 + outgoing 2) - SRAM 400KB 제약
- **BLE 스텍**: Bluedroid 사용 (Arduino BLE 라이브러리 호환).
  C3의 기본 스텍인 NimBLE보다 메모리를 더 쓰지만, API 호환성을 위해 강제.
- **패킷 최대 크기**: 1,168바이트 (BLE MTU 517B + 흐름제어). 현재 512B
  페이로드 제한으로 충분히 여유. C3에서는 더 큰 패킷 시 BLE 메모리 부족 위험.
- **Ed25519 검증 오버헤드**:
  - ESP32 클래식: 약 30ms/패킷 (240MHz 듀얼코어)
  - ESP32-C3: 약 60-80ms/패킷 (160MHz 싱글코어) - 처리량 절반
- **패킷 손실 복구**: ACK만 처리하고 재전송 로직은 미구현.
- **인터넷 브릿지**: WiFi/MQTT 브릿지는 미구현. BLE↔BLE 메시만 지원.

## 보드별 빌드 매크로

소스 코드는 동일하지만 빌드 시점에 매크로로 보드 특화 값을 자동 조정:

| 매크로 | 정의 시점 | 영향 |
|--------|----------|------|
| `BOARD_VARIANT_ESP32` | platformio.ini env:esp32dev (명시적) | 기본값 (듀얼코어, 7 연결) |
| `BOARD_VARIANT_ESP32C3` | platformio.ini env:esp32c3 (명시적) | 4 연결, 8 큐 |
| `CONFIG_IDF_TARGET_ESP32C3` | ESP-IDF `set-target esp32c3` (자동) / Arduino IDE C3 보드 선택 (자동) | 동일 |
| `BC_BOARD_IS_C3` | 위 어느 것이든 정의되면 자동 정의 | C3 최적화 적용 |

3개 빌드 시스템 모두 C3 보드로 빌드할 때 자동으로 C3 최적화가 적용됩니다:

- **PlatformIO**: env:esp32c3 선택 시 `BOARD_VARIANT_ESP32C3=1` 자동 정의
- **ESP-IDF**: `idf.py set-target esp32c3` 시 `CONFIG_IDF_TARGET_ESP32C3` 자동 정의
- **Arduino IDE**: Tools > Board에서 C3 보드 선택 시 `CONFIG_IDF_TARGET_ESP32C3` 자동 정의
  (ESP-IDF를 backend로 사용하므로 동일한 매크로)

C3 최적화가 적용되면:
- `BC_MAX_OUT_CONN` 3 → 2 (outgoing 클라이언트 연결 수)
- `BC_MAX_IN_CONN` 4 → 2 (incoming 서버 연결 수)
- `BC_PKT_QUEUE_LEN` 16 → 8 (메모리 절약)
- `sdkconfig.defaults.esp32c3`의 BLE unicore/NimBLE 비활성/동적 메모리 설정이 반영

---

## 향후 확장 아이디어

- WiFi/MQTT 브릿지: 오프라인 메시 ↔ 인터넷 클라우드 연동
- OLED 디스플레이: 노드 ID, 카운터, RSSI 시각화
- LoRa 모듈 (SX1276) 추가: BLE ↔ LoRa 장거리 브릿지
- 화이트리스트/블랙리스트 필터링 (피어 PK 기반)
- 딥슬립 배터리 절전 모드
- OTA 펌웨어 업데이트

---

## 라이선스

MIT License - 자유롭게 사용/수정/배포 가능.

## 참고 자료

- [bitchat](https://github.com/dim069/bitchat) - 원본 bitchat 프로젝트
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/)
- [mbedtls Ed25519 docs](https://www.trustedfirmware.org/projects/mbedtls/)
- [Bluetooth GATT specifications](https://www.bluetooth.com/specifications/gatt/)
