# bitchat ESP32 Relay Firmware

> ESP32 클래식 / ESP32-C3 / ESP32-S3 기반의 bitchat 메시 중계 노드 펌웨어

이 펌웨어는 ESP32를 bitchat BLE 메시의 중계(relay) 노드로 동작시킵니다. 주변 bitchat BLE 노드(스마트폰, 다른 ESP32 등)가 보낸 패킷을 수신해서 서명을 검증한 뒤 다른 인접 노드로 전달합니다. 인터넷이나 셀룰러 없이 오프라인으로 동작하며, 메시 커버리지를 확장합니다.

추가로 **LoRa 모듈 자동 감지** + **WiFi/MQTT 브릿지**를 통한 BLE↔LoRa↔인터넷 양방향 브릿지를 지원합니다.

---

## 지원 보드

| 항목 | ESP32 클래식 | ESP32-C3 | ESP32-S3 |
|------|--------------|----------|----------|
| 칩 | ESP-WROOM-32 | ESP32-C3 | ESP32-S3 |
| 코어 | Xtensa LX6 듀얼코어 240MHz | RISC-V RV32 싱글코어 160MHz | Xtensa LX7 듀얼코어 240MHz |
| SRAM | 520 KB | 400 KB | 512 KB + PSRAM 옵션 |
| Flash | 4 MB 이상 | 4 MB (일반적) | 8 MB (표준) |
| BLE | 4.2 | 5.0 | 5.0 |
| WiFi | 802.11 b/g/n | 802.11 b/g/n | 802.11 b/g/n |
| USB | 외부 브릿지 (CP2102/CH340) | CDC 내장 | OTG 내장 |
| 동시 BLE 연결 (본 fw) | 8 (in 4 + out 4) | 8 (in 4 + out 4) | 8 (in 4 + out 4) |
| 전원 | USB 5V 상시 또는 배터리 | USB 5V 상시 또는 배터리 | USB 5V 상시 또는 배터리 |
| 추천 용도 | 메시 허브 (표준) | 저전력 소형 노드 / 배터리 운용 | 메시 허브 + USB OTG + PSRAM (최고 성능) |

세 보드 모두 동일한 소스 코드로 빌드되며, 빌드 시 `BOARD_VARIANT_*` 매크로로 자동 감지되어 연결 수와 큐 크기가 보드별 최적화됩니다.

---

## 주요 기능

| 항목 | 구현 내용 |
|------|----------|
| BLE GATT 서버 | 자체 서비스 UUID(`0000bc00-...`)로 advertise, 클라이언트 write 수신 |
| BLE GATT 클라이언트 | 동시 스캐너로 주변 bitchat 노드 발견 시 자동 연결 + notify 구독 |
| Ed25519 서명 검증 | libsodium 기반, 모든 수신 패킷 서명 검증 후에만 중계 |
| NVS 영속화 | 자체 Ed25519 키쌍, 카운터, 피어 목록, WiFi 자격증명을 플래시에 저장 |
| 중복 패킷 dedup | LRU 64-슬롯 캐시로 30초 이내 중복 무시 (메시 루프 방지) |
| TTL 감소 | 중계 시마다 TTL 1 감소, TTL=0이면 더 이상 전달 안 함 (최대 7홉) |
| ACK 재전송 | 패시브 ACK + 명시적 ACK(type=0x02) + 타임아웃 재전송 (최대 3회) |
| 블랙리스트 | Ed25519 공개키 + BLE 주소 차단, NVS 영속화, Serial 명령 관리 |
| Serial UART 로그 | 115200bps로 텍스트 + 패킷 hex dump + Serial 명령 인터페이스 |
| **배터리+절전 모드** | ESP-PM 라이트 슬립 + BLE 저듀티 + LED 펄스 + 딥슬립 (GPIO 0 wake) |
| **NimBLE 스텍** | Bluedroid 대신 NimBLE-Arduino 사용, 메모리 1/3 절약 |
| **LoRa 자동감지** | SX127x 모듈 자동 인식 (Heltec, TTGO 등 4가지 핀아웃 시도) |
| **WiFi/MQTT 브릿지** | Serial 'wifi on'으로 활성화, MQTT bchat/tx ↔ bchat/rx/{node_id} 양방향 |
| **BLE Optimum coex** | WiFi + BLE 동시 사용 시 BLE pairing 안정성 최우선 모드 |
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
| 라이트 슬립 (BLE on) | ESP32-S3 | 8-12 mA | 250-375 시간 (10-15일) |
| 딥슬립 (5분 무활동) | ESP32 클래식 | 0.18 mA | 16,000 시간 (약 2년) |
| 딥슬립 (5분 무활동) | ESP32-C3 | 0.005 mA (RTC) | ~70,000 시간 (약 8년) |
| 딥슬립 (5분 무활동) | ESP32-S3 | 0.18 mA | 약 2년 |
| USB 상시 풀 가동 | ESP32 클래식 | 60-80 mA | 37-50 시간 (1.5-2일) |
| USB 상시 풀 가동 | ESP32-C3 | 35-50 mA | 60-85 시간 (2.5-3.5일) |
| USB 상시 풀 가동 | ESP32-S3 | 60-80 mA | 37-50 시간 (1.5-2일) |

> **참고**: 실제 동작 시간은 BLE 연결 수, 패킷 빈도, 온도, 배터리 상태, WiFi 활성화 여부에 따라 달라집니다.

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

ESP32-S3 (DevKitC-1)
├── GPIO 48  ─── LED (온보드 RGB - WS2812)
├── GPIO 0   ─── BOOT 버튼 (딥슬립 wake)
├── GPIO 4   ─── 배터리 ADC (분배 2:1)
└── 3.3V     ─── LDO → 배터리 (+)
```

> S3의 온보드 LED는 WS2812 (RGB)라 simple digitalWrite로는 안 켜질 수 있음. `BC_LED_PIN`을 일반 GPIO로 변경 권장.

### 빌드 환경별 모드 선택

PlatformIO에서는 env 이름으로 선택:

```bash
# 배터리 모드 (기본)
pio run -e esp32dev       # ESP32 클래식 + 배터리
pio run -e esp32c3        # ESP32-C3 + 배터리
pio run -e esp32s3        # ESP32-S3 + 배터리

# USB 상시 전원 모드 (이전 동작)
pio run -e esp32dev-usb   # ESP32 클래식 + USB
pio run -e esp32c3-usb    # ESP32-C3 + USB
```

ESP-IDF에서는 `idf.py menuconfig`에서 `CONFIG_PM_ENABLE`를 켜면 자동으로 배터리 모드 활성화.

Arduino IDE에서는 build flags에 `-DBC_POWER_BATTERY=1`을 추가 (또는 생략 시 자동 배터리 모드).

---

## 하드웨어 요구사항

- **지원 보드**:
  - **ESP32 클래식**: ESP-WROOM-32, DevKitC V4, NodeMCU-32S, Wemos D1 R32 등
  - **ESP32-C3**: DevKitM-1, Seeed XIAO ESP32C3, ESP32-C3-DevKitC-02 등
  - **ESP32-S3**: DevKitC-1, ESP32-S3-WROOM-1, Seeed XIAO ESP32S3 등
- **플래시**: 4MB 이상 (기본 파티션 테이블 사용), S3는 8MB 표준
- **전원**: USB 5V 상시 또는 LiPo 배터리 (3.7V)
- **추가 모듈**: 불필요 (BLE 온칩으로 충분)
- **옵션 LoRa**: SX1276/77/78 또는 RFM95W 보드 (Heltec WiFi LoRa 32, TTGO T-Beam 등 통합 보드 자동 감지)
- **시리얼**: USB-Serial 브릿지 (CP2102/CH340) 또는 C3/S3의 내장 USB CDC/OTG

### 핀 연결

별도 GPIO 핀 사용 없음. USB Serial만 사용:
- TX/RX는 보드에 내장된 USB-Serial 브릿지를 통해 PC와 연결
- BLE 안테나는 ESP32 모듈에 PCB 내장
- LoRa 모듈 자동 감지 (있을 경우 SPI 핀 자동 매핑)

---

## 패킷 프로토콜 구조

bitchat 호환 패킷 포맷 (빅엔디언):

```
+------+------+------+------+----------------------+
|magic | ver  | type | ttl  | sender_pk (32 bytes)  |
| 0xBC | 0x01 | 1B   | 1B  | (Ed25519 공개키)        |
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

가장 빠르고 의존성 관리가 자동입니다. 세 보드 모두 지원.

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

# ESP32-S3 빌드
pio run -e esp32s3
pio run -t upload -e esp32s3
pio device monitor -e esp32s3
```

**VS Code PlatformIO 확장**을 쓰면 하단 바에서 Build/Upload/Monitor 버튼을 클릭만 하면 됩니다. 환경 선택은 하단 바의 env 선택기(`esp32dev`, `esp32c3`, `esp32s3`)를 클릭하여 전환합니다.

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

# ===== ESP32-S3 빌드 =====
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`sdkconfig.defaults`가 공통 설정을, `sdkconfig.defaults.esp32c3`가 C3 전용 오버라이드(BLE 동시 연결, FreeRTOS unicore, 메모리 절약)를 담당합니다. `idf.py set-target`이 자동으로 적합한 defaults를 병합합니다.

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
   - ESP32-S3: `ESP32S3 Dev Module` (또는 `Seeed XIAO ESP32S3`)
3. **Tools > Upload Speed** = `921600` (C3/S3 USB CDC 보드는 `115200`로 낮출 것)
4. **Tools > Port** = `/dev/ttyUSB0` (Linux) 또는 `COM3` (Windows)
5. **Upload** 버튼 클릭
6. **Tools > Serial Monitor** 열기, baud를 115200으로 설정

> **참고**: 보드를 선택하면 `CONFIG_IDF_TARGET_ESP32C3`/`ESP32S3` 매크로가 자동 정의되어 소스가 보드별 설정(연결 수, 큐 크기)으로 자동 조정됩니다.

---

## 시리얼 로그 예시

부팅 후 다음과 같은 로그가 115200bps로 출력됩니다:

```
=============================
 bitchat ESP32 relay firmware
 build: Oct 28 2026 02:50:00
 target: ESP32-S3 (Xtensa LX7, BLE 5.0 + USB-OTG)
 max-conn: in=4 out=4 queue=16
 power: BATTERY + light/deep sleep
=============================

[I][bitchat] crypto: libsodium ready
[I][bitchat] === storage dump ===
[I][bitchat] self_id=8a3f2c1b9e04 ready=1 counter=0 peers=0
[I][bitchat] BLE advertising started as bchat-relay (NimBLE)
[I][bitchat] BitchatRelay ready (NimBLE backend). max out=4 in=4
[I][bitchat] === bitchat relay node ready ===
[I][bitchat] lora: scanning for SX127x module (4 pinout candidates)...
[I][bitchat] lora: ✓ Heltec/TTGO LoRa32 V1/V2 detected (SX1276, version=0x12)
[I][bitchat] LoRa bridge ENABLED: Heltec/TTGO LoRa32 V1/V2
[I][bitchat] WiFi bridge ready (use 'wifi on' to connect)

[I][bitchat] --- status: in=0 out=0 ctr=0 peers=0 ---
[I][bitchat] --- retry: pending=0 retrans=0 p_ack=0 e_ack=0 dropped=0 ---
[I][bitchat] --- blacklist: pk=0 ble=0 enabled=1 ---
[I][bitchat] === lora dump ===
[I][bitchat] stats: rx=0 tx=0 drop_large=0 drop_parse=0
```

---

## Serial 명령 인터페이스

USB Serial Monitor에서 입력 가능한 명령:

### 블랙리스트 관리

```text
> bl                       # 현재 블랙리스트 dump
> bl stats                 # 카운트만 출력
> bl on                    # 블랙리스트 활성화 (기본)
> bl off                   # 블랙리스트 비활성화 (모든 체크 skip)
> bl add 8a3f2c1b9e04...   # Ed25519 pk 추가 (32B = 64 hex chars)
> bl add aabbccddeeff      # BLE 주소 추가 (6B = 12 hex chars)
> bl rm <hex>              # 제거
> bl clear                 # 전체 초기화
```

### WiFi 브릿지 제어

```text
> wifi                     # WiFi+MQTT 상태
> wifi on                  # NVS 또는 컴파일 타임 매크로로 연결
> wifi on <ssid> <pass>    # 지정한 SSID/pass로 연결
> wifi off                 # WiFi+MQTT 연결 해제 (BLE 우선 모드로 전환)
> wifi set <ssid> <pass>   # NVS에 영속 저장 (재부팅 후 'wifi on'에서 자동 사용)
> wifi stat                # 통계만 출력
```

### 기타

```text
> help                     # 도움말
```

---

## ACK 재전송 로직 (retry_queue)

`src/retry_queue.h/.cpp`에 구현된 메시 프로토콜의 핵심 신뢰성 메커니즘.

### 두 가지 ACK 메커니즘

1. **패시브 ACK**: 우리가 forward한 패킷이 다른 peer에 의해 재송신되는 것을 감지하면 우리 forward가 성공했다고 간주하고 재전송 큐에서 제거 (BLE mesh, Thread, Zigbee 표준 패턴)
2. **명시적 ACK (type=0x02)**: bitchat ACK 패킷의 payload에서 원본 (sender_pk, counter)를 추출하여 매칭. 최종 목적지 도달을 의미

### 설정값

| 매크로 | 기본값 | 의미 |
|--------|--------|------|
| `BC_RETRY_QUEUE_SIZE` | 16 | 동시 추적할 pending entry 수 (16 × 623B = 10KB RAM) |
| `BC_RETRY_TIMEOUT_MS` | 2000 | 단일 홉 ACK 대기 시간 (2초) |
| `BC_RETRY_MAX_RETRIES` | 3 | 최대 재시도 횟수 (총 4회 송신) |

---

## 블랙리스트 필터링 (blacklist)

`src/blacklist.h/.cpp`에 두 가지 차단 메커니즘 구현:

1. **Ed25519 공개키 블랙리스트 (32B × 32개)** - bitchat 노드 정식 식별자로 차단
2. **BLE 주소 블랙리스트 (6B × 16개)** - advertising 단계에서 차단 (연결 시도 안 함)

모든 블랙리스트는 NVS에 영속화되어 재부팅 후에도 유지. Serial 명령으로 실시간 add/remove 가능.

---

## NimBLE BLE 스텍 (Bluedroid 대안)

본 펌웨어는 **NimBLE-Arduino** (h2zero) 라이브러리를 사용하여 ESP-IDF의 기본 Bluedroid 대신 더 가벼운 NimBLE 스텍을 사용합니다.

### NimBLE vs Bluedroid 장점

| 항목 | Bluedroid | NimBLE | 효과 |
|------|-----------|--------|------|
| 코드 크기 (Flash) | 큼 (≈500KB+ libs) | 작음 (≈250KB) | Flash 30-40% 절약 |
| RAM 사용량 | 커넥션당 5-7KB | 커넥션당 1-2KB | 동일 RAM으로 2-3배 많은 연결 |
| 모든 보드 동시 연결 | 4-7 (SRAM 제약) | 8-9 (heap 여유) | 안전 마진 1개 |
| BLE 5.0 | 미지원 (4.2 클래식) | 지원 | Extended adv 등 (C3/S3) |

### 빌드 크기 비교 (NimBLE + LoRa + WiFi 포함, 배터리 모드)

| 보드 | RAM | Flash | 비고 |
|------|-----|-------|------|
| ESP32 클래식 | 22.3% (73KB) | 92.2% (1.21MB) | ⚠️ huge_app partition 권장 |
| ESP32-C3 | 19.6% (64KB) | 86.7% (1.14MB) | 안정적 |
| ESP32-S3 | 21.6% (71KB) | 84.0% (1.10MB) | 가장 여유 (8MB Flash) |

---

## BLE 동시 연결 한계

### ESP32 BLE 컨트롤러 하드웨어 한계

| 보드 | BLE 컨트롤러 최대 연결 | 비고 |
|------|----------------------|------|
| ESP32 클래식 | **9개** (silicon limit) | 칩 자체 한계, 소프트웨어로 못 바꿈 |
| ESP32-C3 | **9개** | 동일 |
| ESP32-S3 | **9개** | 동일 |

### 본 펌웨어의 안전 설정

**NimBLE 9 + 앱 8 (4+4) = 안전 마진 1개**:

```c
// lib/NimBLE-Arduino/src/nimconfig.h
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 9    // 컨트롤러 한계 = NimBLE 한계

// src/ble_relay.h
#define BC_MAX_OUT_CONN  4    // outgoing (클라이언트로 연결)
#define BC_MAX_IN_CONN   4    // incoming (서버로 받음)
// 총 8 + NimBLE 버퍼 1개 headroom = 9 (컨트롤러 한계 일치)
```

ESP32 칩 하나당 9개가 절대 한계이므로, **10+ 연결이 필요하면 다중 ESP32 노드로 분산**해야 합니다.

### 10+ 연결이 필요한 시나리오 해결책

```
[스마트폰 5대]                  [스마트폰 5대]
       │                              │
   [ESP32-A]  ←— BLE 메시 —→  [ESP32-B]
   max 8 conn                   max 8 conn
       │
   [ESP32-C] ← max 8 conn
```

- 각 ESP32 노드가 8개씩 처리 → 3대로 24개 연결까지 확장
- 노드 간은 BLE 메시로 자동 forwarding (본 펌웨어가 처리)

---

## LoRa 모듈 자동 감지 + BLE↔LoRa 브릿지

`src/lora_relay.cpp`에서 arduino-LoRa (Sandeep Mistry) 라이브러리 기반으로 SX127x 시리즈 LoRa 모듈을 자동 감지합니다.

### 자동 감지 핀아웃 후보

| 후보 | SCK | MISO | MOSI | CS | RST | IRQ | 호환 보드 |
|------|-----|------|------|----|----|------|-----------|
| 1 | 5 | 19 | 27 | 18 | 14 | 26 | Heltec WiFi LoRa 32 V1/V2, TTGO LoRa32 V1 |
| 2 | 5 | 19 | 27 | 18 | 23 | 26 | TTGO T-Beam |
| 3 | 18 | 19 | 23 | 5 | 14 | 26 | Dragino Lora32 / Generic |
| 4 | 18 | 19 | 23 | 5 | 14 | 33 | Alternative CS/RST/IRQ |

### LoRa 설정 (기본값)

| 항목 | 기본값 |
|------|--------|
| Frequency | 921 MHz (한국 KR 대역, 920-923MHz) |
| Bandwidth | 125 kHz |
| Spreading Factor | 7 (빠른 전송) |
| Coding Rate | 4/5 |
| TX Power | 17 dBm |
| Sync Word | 0x34 (bitchat-LoRa 전용) |
| CRC | enabled |

### 한계

- **SX127x만 지원**: SX1262/1268 (Heltec V3 등)은 RadioLib 라이브러리 필요
- **단일 채널**: FHSS 미구현
- **큰 패킷 drop**: LoRa 프레임 최대 222B. bitchat 패킷이 초과 시 drop
- **듀티 사이클**: 한국 920MHz 대역은 5mW 이하 출력 제한 → 17dBm 사용 시 규제 확인 필요

---

## WiFi/MQTT 브릿지 (BLE↔인터넷)

`src/wifi_bridge.cpp`에서 WiFi STA + MQTT 클라이언트로 BLE 메시 패킷을 인터넷으로 브릿지합니다.

### BLE + WiFi 동시 사용 (BLE Optimum coex)

`sdkconfig.defaults`에서 BLE pairing 안정성 최우선 모드로 설정:

```
CONFIG_ESP_WIFI_ENABLED=y
CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE=y
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
CONFIG_ESP32_WIFI_SOFTAP_SUPPORT=n   # STA만 사용
```

- BLE pairing/IoT 기기 안정성 최우선
- WiFi가 BLE에 간섭 안 일으키도록 BLE에 더 많은 라디오 슬롯 할당
- WiFi heavy 트래픽에도 BLE 연결 안정성 유지

### MQTT 토픽

- `bchat/tx` - publish (다른 노드가 subscribe)
- `bchat/rx/{node_id}` - subscribe (이 노드로 들어오는 패킷)

### 동작 흐름

```
[BLE 패킷 수신]              [LoRa 패킷 수신]            [MQTT 패킷 수신]
       │                          │                          │
       ▼                          ▼                          ▼
  process_packet           onReceive callback          mqtt_callback
       │                          │                          │
       └── forward_to_others ────┴──────────────────────────┘
              ├─ ★ LoRa TX (broadcast)
              ├─ ★ WiFi/MQTT publish (bchat/tx)
              ├─ BLE notify (incoming clients)
              └─ BLE write (outgoing peers)
```

### Serial 명령 (실시간 ON/OFF)

```text
> wifi on    # WiFi+MQTT 연결 시도
> wifi off   # 연결 해제 (BLE 우선 모드로 전환)
> wifi set <ssid> <pass>   # NVS 영속 저장
```

### 한계

- **평문 MQTT** (1883 포트) - TLS 인증서 미지원 (간단함 우선)
- **WiFi 자동 연결 안 함**: 사용자가 명시적으로 `wifi on` 필요 (BLE 우선 정책)
- **ESP32 클래식 Flash 92.2%** - WiFi 활성화로 Flash 증가, `huge_app.csv` partition 권장

---

## 파일 구조

```
bitchat-esp32-relay/
├── README.md                      # 이 파일
├── platformio.ini                 # PlatformIO 빌드 (5 envs: esp32dev/c3/s3 + USB 변형)
├── CMakeLists.txt                 # ESP-IDF 루트
├── sdkconfig.defaults             # ESP-IDF 기본 (ESP32 공통 + PM/tickless + BLE coex)
├── sdkconfig.defaults.esp32c3     # ESP-IDF C3 전용 오버라이드
├── bitchat_relay.ino              # Arduino IDE 진입 sketch (루트)
├── setup_arduino_ide.sh           # Arduino IDE 폴더 자동 생성 스크립트
├── src/                           # 표준 소스 폴더 (20개 파일)
│   ├── main.cpp                   # Arduino 진입 + Serial 명령 + WiFi 초기화
│   ├── log.h                      # Serial 로깅 매크로 (BC_LOG_LEVEL_*)
│   ├── bitchat_protocol.h/.cpp    # 패킷 직렬화/검증
│   ├── crypto.h/.cpp              # Ed25519 (libsodium)
│   ├── storage.h/.cpp             # NVS 키/피어 영속화
│   ├── power.h/.cpp               # ESP-PM 라이트 슬립 + 배터리 ADC + LED 펄스
│   ├── retry_queue.h/.cpp         # ACK 재전송 (패시브 ACK + 타임아웃)
│   ├── blacklist.h/.cpp           # pk/BLE 주소 블랙리스트 (NVS 영속화)
│   ├── lora_relay.h/.cpp          # LoRa 자동감지 + BLE↔LoRa 브릿지 (SX127x)
│   ├── wifi_bridge.h/.cpp         # WiFi+MQTT 브릿지 (BLE↔인터넷, Serial on/off)
│   └── ble_relay.h/.cpp           # BLE GATT + relay (NimBLE, 배터리 모드 시 저듀티)
├── lib/                           # 외부 라이브러리 (자체 포함)
│   ├── NimBLE-Arduino/            # h2zero NimBLE-Arduino 1.4.2
│   ├── LoRa/                      # Sandeep Mistry arduino-LoRa 0.8.0
│   └── PubSubClient/              # knolleary PubSubClient 2.8 (MQTT)
├── main/
│   └── CMakeLists.txt             # ESP-IDF main 컴포넌트 (../src/ 참조)
├── firmware/                      # 빌드된 바이너리 (3개 보드)
│   ├── esp32dev-battery.bin       # ESP32 클래식 펌웨어
│   ├── esp32c3-battery.bin        # ESP32-C3 펌웨어
│   ├── esp32s3-battery.bin        # ESP32-S3 펌웨어
│   └── bootloader/partitions*.bin
└── components/                    # ESP-IDF용 외부 컴포넌트 (arduino-esp32)
```

---

## 동작 원리 요약

1. **부팅 시**: NVS에서 자체 Ed25519 키쌍을 복원. 없으면 새로 생성 후 저장.
2. **BLE 서버**: `0000bc00-...` 서비스로 advertise. 클라이언트 write용 RX char,
   notify용 TX char 제공.
3. **BLE 클라이언트**: 동시에 scanner를 돌려 같은 서비스를 advertise하는
   주변 노드 발견. 자리가 남으면 자동으로 연결 + notify 구독.
4. **LoRa 자동감지**: 4가지 핀아웃 후보 순차 시도 → SX127x 정상 응답 시 활성화.
5. **WiFi 브릿지**: Serial 'wifi on' 시 WiFi+MQTT 연결, BLE 패킷을 MQTT로도 브로드캐스트.
6. **패킷 수신** (BLE / LoRa / MQTT 어디든):
   - magic/version/TTL/payload_len 구조 검증
   - 블랙리스트 체크 (sender_pk가 차단되었는지)
   - 패시브 ACK 체크 (이미 forward한 패킷인지)
   - Ed25519 서명 검증
   - LRU dedup 캐시 확인 (30초 이내 중복 무시)
   - 피어 캐시에 sender_pk 추가
   - TTL 1 감소 (TTL=0이면 더 이상 forward 안 함)
   - BLE notify + LoRa broadcast + MQTT publish
   - retry_queue에 추가 (ACK 대기)

---

## 제한사항

- **ESP32 클래식 Flash 92.2%** - WiFi 활성화로 위험 수준. `huge_app.csv` 파티션 전환 권장
- **C3/S3/Classic 모두 BLE 동시 연결 8개** (NimBLE max=9 + 앱 1 headroom)
- **ESP32 BLE 컨트롤러 하드 한계 9개** - 10+ 연결 필요 시 다중 ESP32 분산
- **Ed25519 검증 속도**: 클래식/S3 ~30ms (듀얼코어 240MHz), C3 ~60-80ms (싱글코어 160MHz)
- **WiFi + BLE 동시 사용 시**: WiFi throughput 30-50% 감소 가능 (Optimum coex로 BLE 안정성 유지)
- **LoRa SX127x만 지원**: SX1262/1268 (Heltec V3)은 RadioLib 필요
- **평문 MQTT** (1883 포트, TLS 미지원)
- **NimBLE 1.4.2 API 고정**: 최신 NimBLE-Arduino 2.x/3.x와 API 다름
- **MTU 247B (NimBLE 기본)**: 큰 패킷 처리 시 BLE fragmentation 필요
- **ESP32-C6 미지원**: arduino-esp32 v2.0.14에 C6 variant 없음 (v3.x 필요)

---

## 보드별 추천 시나리오

| 시나리오 | 추천 보드 | 이유 |
|----------|-----------|------|
| 가정용 소규모 (4명 이하) | ESP32-C3 | 저전력, 배터리 20-31일 |
| 소규모 회의 (8명) | ESP32-S3 | 8 conn 수용, 8MB Flash 여유 |
| 메시 허브 + LoRa 장거리 | ESP32-S3 + Heltec LoRa | 듀얼코어, BLE 5.0, USB OTG |
| 배터리 종단 노드 (저전력) | ESP32-C3 | RISC-V 싱글코어 저전력 |
| WiFi 브릿지 + BLE 메시 | ESP32-S3 | Flash 84% (여유), Optimum coex 안정 |
| 대규모 이벤트 (50명+) | 다중 ESP32-S3 분산 | 단일 칩 9개 한계 → 노드 추가 |
| 선형 메시 (트레일/산책로) | ESP32-C3 + LoRa | 저전력 + 장거리 조합 |

---

## 향후 확장 아이디어

- **ESP32-C6 지원**: arduino-esp32 v3.x로 마이그레이션 (BLE 5.3 + WiFi 6 + Thread)
- **종단간 암호화**: ECDH 키 교환 + AES-GCM payload 암호화
- **TLS MQTT**: mTLS 인증서로 보안 MQTT 브로커 연결
- **OLED 디스플레이**: 노드 ID, 카운터, RSSI 시각화 (SSD1306 I2C)
- **OTA 펌웨어 업데이트**: WiFi 연결 시 무선 업데이트
- **NVS Encryption**: `CONFIG_NVS_ENCRYPTION=y`로 키 저장소 보안 강화
- **BLE 5.0 Extended Advertising**: C3/S3에서 긴 광고 데이터 (255B → 1650B)
- **큰 메시지 분할**: 222B 초과 패킷의 LoRa fragmentation

---

## 라이선스

MIT License - 자유롭게 사용/수정/배포 가능.

## 참고 자료

- [bitchat](https://github.com/dim069/bitchat) - 원본 bitchat 프로젝트
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) - BLE 라이브러리
- [arduino-LoRa](https://github.com/sandeepmistry/arduino-LoRa) - SX127x 라이브러리
- [PubSubClient](https://github.com/knolleary/pubsubclient) - MQTT 클라이언트
- [libsodium](https://libsodium.gitbook.io/) - Ed25519 + crypto primitives
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/)
- [Bluetooth GATT specifications](https://www.bluetooth.com/specifications/gatt/)
