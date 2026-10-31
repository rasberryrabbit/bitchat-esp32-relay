# bitchat ESP32 Relay Firmware

> ESP32 클래식 / ESP32-C3 / ESP32-S3 기반의 bitchat 메시 중계 노드 펌웨어

[English](README_EN.md) | **한국어**

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
| 블랙리스트 | Ed25519 공개키(256개) + BLE 주소(128개) 차단, **SPIFFS 영속화**, Serial 명령 관리 |
| Serial UART 로그 | 115200bps로 텍스트 + 패킷 hex dump + Serial 명령 인터페이스 |
| **배터리+절전 모드** | ESP-PM 라이트 슬립 + BLE 저듀티 + LED 펄스 + 딥슬립 (GPIO 0 wake) |
| **NimBLE 스텍** | Bluedroid 대신 NimBLE-Arduino 사용, 메모리 1/3 절약 |
| **LoRa 자동감지** | SX127x 모듈 자동 인식 (Heltec, TTGO 등 4가지 핀아웃 시도) |
| **큰 패킷 분할/재조립** | LoRa 222B 한계 우회 - bitchat 패킷(최대 623B)을 16 청크로 분할 후 재조립 |
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
├── GPIO 5   ─── 외부 LED (온보드 RGB-WS2812는 digitalWrite 미지원 → GPIO 5 사용)
│               (SPI 10-14, Flash 6-11, USB 19-20, UART 43-44, Strapping 0/3/45/46과 무관)
├── GPIO 0   ─── BOOT 버튼 (딥슬립 wake)
├── GPIO 4   ─── 배터리 ADC (분배 2:1)
└── 3.3V     ─── LDO → 배터리 (+)
```

> S3의 온보드 RGB LED(GPIO 48)는 WS2812라 simple digitalWrite로 제어 불가 → 외부 LED를 GPIO 5에 연결 권장. GPIO 5는 모든 SPI/Flash/USB/UART/Strapping 핀과 무관하여 안전.

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

# LoRa 통합 보드 전용 (보드별 LED/배터리 핀 매핑 자동 적용)
pio run -e heltec_v2      # Heltec WiFi LoRa 32 V2 / TTGO LoRa32 V1.x (LED=25, BAT_ADC=13)
pio run -e heltec_v2-usb  # Heltec V2 + USB 상시
pio run -e t-beam         # TTGO T-Beam V1.x (AXP192 PMU 경유 - LED/ADC 비활성)
pio run -e t-beam-usb     # T-Beam + USB 상시
```

> **LoRa 통합 보드 사용 시 참고**:
> - Heltec V2는 LED가 GPIO 25에 있습니다 (GPIO 2 아님). `heltec_v2` env를 쓰면 자동으로 매핑됩니다.
> - Heltec V2의 배터리 전압 분배는 GPIO 13 (ADC2)에 있습니다. **WiFi bridge(`wifi on` 명령)를 활성화하면 ADC2가 동작하지 않아 배터리 측정이 무효**합니다. BLE-only 운영 시에만 유효합니다.
> - TTGO T-Beam V1.x는 배터리 전압과 LED가 모두 AXP192 PMU (I2C addr 0x34)를 통해 제어됩니다. `t-beam` env에서는 LED/ADC를 모두 비활성화(`0xFF`)합니다. **정확한 배터리 측정 + LED 제어를 원하면 AXP192 I2C 드라이버 추가가 필요합니다** (TODO, 현재 미구현).
> - LoRa 자동 감지는 Heltec V2와 T-Beam 모두 동일한 SPI 핀(SCK=5, MISO=19, MOSI=27, CS=18)을 공유하지만 RST 핀만 다릅니다(Heltec=14, T-Beam=23). `lora_relay_begin()`이 두 후보를 순차 시도하여 첫 번째로 인식되는 모듈을 사용합니다.

ESP-IDF에서는 `idf.py menuconfig`에서 `CONFIG_PM_ENABLE`를 켜면 자동으로 배터리 모드 활성화.

Arduino IDE에서는 build flags에 `-DBC_POWER_BATTERY=1`을 추가 (또는 생략 시 자동 배터리 모드).

Arduino IDE에서 Heltec V2를 빌드하려면 Tools > Custom Build Properties에 다음을 추가:
```
-DDBC_LED_PIN=25
-DDBC_BATTERY_ADC_PIN=13
```
T-Beam에서는 대신:
```
-DDBC_LED_PIN=255
-DDBC_BATTERY_ADC_PIN=255
```

---

## 하드웨어 요구사항

- **지원 보드**:
  - **ESP32 클래식**: ESP-WROOM-32, DevKitC V4, NodeMCU-32S, Wemos D1 R32 등
  - **ESP32-C3**: DevKitM-1, Seeed XIAO ESP32C3, ESP32-C3-DevKitC-02 등
  - **ESP32-S3**: DevKitC-1, ESP32-S3-WROOM-1, Seeed XIAO ESP32S3 등
- **플래시**: 4MB (기본) - 파티션 테이블 `partitions/no_ota_spiffs_1mb.csv` 사용
  - app0 2,848KB (OTA 제거로 확장, 펌웨어 ~1.2MB → 42% 사용)
  - SPIFFS 1024KB (블랙리스트 9KB + 1,015KB 여유, 메시지 캐시/설정 저장용)
  - NVS 24KB (자체 키/피어/카운터 + bat_ignore 플래그), coredump 64KB
  - S3는 8MB 표준이지만 동일 파티션 레이아웃 사용
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

1. **Ed25519 공개키 블랙리스트 (32B × 256개)** - bitchat 노드 정식 식별자로 차단
2. **BLE 주소 블랙리스트 (6B × 128개)** - advertising 단계에서 차단 (연결 시도 안 함)

모든 블랙리스트는 **SPIFFS 파일**(`/blacklist_pk.bin`, `/blacklist_ble.bin`)에 영속화되어 재부팅 후에도 유지. Serial 명령으로 실시간 add/remove 가능.

### 백엔드 전환: NVS → SPIFFS

이전 버전에서는 NVS blob으로 저장했으나 다음 한계로 인해 SPIFFS로 전환:
- NVS blob 크기 한계 (~4KB) → pk 32개 + ble 16개가 최대
- 매 add/remove마다 blob 전체 재기록 → flash wear 증가
- 자체 키/피어 캐시와 동일 namespace 공유 → 충돌 위험

**SPIFFS 전환 효과**:
- 용량 8배 확장: pk 32→256개, ble 16→128개
- 파일 단위 저장 → blob 한계 없음, 향후 메타데이터(timestamp, reason) 추가 용이
- NVS는 자체 키/피어 영속화에만 전용 → 관심사 분리
- SPIFFS 1,024KB 파티션 중 9KB만 사용 → 1,015KB 여유

### Store-and-forward 2-tier (RAM + SPIFFS overflow)

메시지 캐시는 RAM 우선, RAM이 가득 찰 때만 SPIFFS로 overflow. 플래시 수명 보호 설계.

| 항목 | RAM cache (msg_cache) | SPIFFS overflow (msg_store) |
|------|----------------------|------------------------------|
| 용량 | 32 entries × 650B = 21KB | **3072 entries × 264B = 792KB** |
| TTL | 12시간 (만료 시 drop) | 부팅 시 RAM으로 복원 시도 (RAM 가득 시 SPIFFS 유지) |
| 쓰기 트리거 | 매 메시지 도착 시 (RAM 빈 슬롯) | RAM 가득 + 직접 메시지 도착 시 |
| 패킷 크기 한계 | 650B (BITCHAT_MAX_PKT_LEN) | 256B (크면 drop) |
| Broadcast 필터 | 모두 캐시 | broadcast는 SPIFFS에 안 씀 → drop |
| 파일 | (RAM 구조체) | 단일 파일 `/msg_store.bin` |
| 부팅 시 동작 | 빈 상태로 시작 | 부팅 시 scan → 다음 write slot + active count 캐싱 |
| 부팅 시 복원 | (RAM은 휘발성) | RAM 빈 슬롯 있으면 복원, 가득 차면 SPIFFS에 유지 |
| 쓰기 성능 | O(1) (RAM 배열) | O(1) (circular buffer, cached write slot) |

**플래시 마모 최소화 전략**:
1. **RAM 우선**: 정상 동작(클라이언트 자주 접속) 시 SPIFFS 쓰기 발생 안 함
2. **직접 메시지만 영속화**: broadcast(브로드캐스트)는 휘발성 → RAM 꽉 차도 drop
3. **단일 파일 + 고정 레코드**: in-place update로 fragment 최소화
4. **3072 슬롯 순환 (circular)**: 한 슬롯에 반복 write 안 함 → wear 분산 (기존 64의 48배)
5. **패킷 크기 한계 256B**: 큰 패킷은 어차피 RAM에 있으니 SPIFFS로 안 옮김
6. **배터리 CRIT 자동 비활성화**: V_CRIT 도달 시 자동으로 `cache spiffs off` 전환 (아래 참고)

**플래시 마모 계산** (worst case, 3072 records):
- 4KB 섹터당 100,000 erase cycle
- 792KB → ~198개 섹터 → 19.8M erase cycle total
- 최악: RAM overflow 1회/분 = 19.8M 분 = 37.7년 (기존 64 슬롯일 때 347일)
- 일반: 10 msgs/day overflow = 5,400년 (사실상 영구)

**쓰기 성능 최적화** (3072 records 대응):
- 부팅 시 1회 full scan (~200-300ms): 다음 write slot + active count 캐싱
- 이후 `msg_store_add()`는 O(1): cached write slot에 직접 seek+write
- `msg_store_count()`도 O(1): RAM 캐시에서 즉시 반환

**동작 흐름**:
1. 부팅 → `msg_store_init()` → SPIFFS 파일 전체 scan → 다음 write slot + active count 캐싱
2. 부팅 → `msg_store_load_to_ram()` → RAM 빈 슬롯에 복원 시도 (최대 32개)
   - 복원 성공한 레코드 → SPIFFS에서 inactive 마킹
   - RAM 가득 차서 못 넣은 레코드 → SPIFFS에 그대로 유지 (나중에 flush 시 전달)
3. 메시지 도착, 클라이언트 없음 → `msg_cache_add()`:
   - RAM 빈 슬롯 있으면 RAM에 추가
   - RAM 가득 찼으면 가장 오래된 entry를 `msg_store_add()`로 SPIFFS에 밀어냄
4. 클라이언트 접속 (2초 후) → `msg_cache_flush(notify_fn)`:
   - RAM의 모든 active entry를 callback으로 방출 (BLE notify)
   - SPIFFS의 모든 레코드도 callback으로 방출
   - SPIFFS 파일 truncate + 캐시 초기화

**Serial 명령**:
```text
cache               # RAM + SPIFFS 상태 출력
cache dump          # 각 슬롯 상세 출력
cache clear         # SPIFFS overflow 파일 강제 삭제
cache spiffs        # SPIFFS 캐시 enable 플래그 상태 출력
cache spiffs on     # SPIFFS overflow 활성화 (기본값, NVS 영속화)
cache spiffs off    # SPIFFS overflow 비활성화 (drop only, NVS 영속화)
```

**`cache spiffs off` 동작**:
- RAM overflow 발생 시 SPIFFS에 write하지 않고 drop만 (플래시 마모 0건)
- 이미 SPIFFS에 있는 레코드는 유지 → 부팅 시 RAM으로 복원, 클라이언트 접속 시 flush됨
- 즉 "새로운 SPIFFS write 금지"만 의미, 기존 데이터는 그대로 전달
- 배터리 부족/플래시 수명 극단 보호/디버깅 시 사용

**`cache spiffs on` 동작** (기본값):
- 정상 동작 복귀, RAM overflow → SPIFFS write 발생

**배터리 CRIT 자동 비활성화**:
- `power_handle_idle()`이 배터리 전압을 1분마다 측정
- V_CRIT (3.10V) 이하로 떨어지면 자동으로 `cache spiffs off` 전환
  - 1회만 실행 (g_auto_disabled_spiffs_at_crit 플래그로 추적)
  - 전압이 CRIT+0.1V 이상으로 회복되면 플래그 리셋 → 다음 CRIT 시 다시 자동 비활성화 가능
- `battery_ignore` 플래그 여부와 무관하게 동작 (BLE는 살려두더라도 SPIFFS write는 막음)
- 로그: `[W] battery CRIT: cache spiffs OFF auto-set (flash wear protection)`
- 사용자가 명시적으로 `cache spiffs on`으로 다시 켤 수 있음 (회복 후)

**적용 시나리오**:
- 일반 운영: 클라이언트가 자주 접속 → RAM 캐시로 충분, SPIFFS는 비어 있음
- 장기 단절 (12시간+): RAM 32개 꽉 참 → overflow direct messages가 SPIFFS로 영속화
- 재부팅 후 클라이언트 접속 → SPIFFS → RAM 복원 → 클라이언트에게 모두 전달

---

## 노드 프로필 + WiFi/MQTT 설정 (SPIFFS config files)

노드 닉네임과 WiFi/MQTT 설정을 SPIFFS 파일(`key=value` 포맷)로 영속화. 기존 NVS 기반 WiFi 자격증명은 부팅 시 자동 마이그레이션 후 SPIFFS로 이관.

### 설정 파일 2개 (SPIFFS 루트)

**`/node.conf`** - 노드 프로필:
```
# bitchat relay node profile
nickname=MyRelayNode
```

**`/wifi.conf`** - WiFi + MQTT 설정:
```
# bitchat relay wifi/mqtt config
ssid=MyHomeWiFi
pass=password123
broker=test.mosquitto.org
port=1883
topic_prefix=bchat
```

### Serial 명령

```text
# 노드 프로필
node                     # 현재 nickname + self_id 출력
node nick <name>         # nickname 설정 (SPIFFS 영속화, 재부팅 필요)
node nick clear          # nickname 제거

# WiFi/MQTT 설정
wifi set <ssid> <pass>   # WiFi 자격증명 (이제 SPIFFS에 저장)
wifi broker <host> [p]   # MQTT broker + port (이제 실제 동작, SPIFFS 영속화)
wifi prefix <prefix>     # MQTT topic prefix (SPIFFS 영속화)
```

### 백엔드 전환: NVS → SPIFFS

| 항목 | 이전 (NVS) | 현재 (SPIFFS) |
|------|-----------|----------------|
| WiFi ssid/pass | NVS blob (bwifi namespace) | `/wifi.conf` 파일 |
| MQTT broker | 컴파일 타임 매크로만 | `/wifi.conf`의 `broker` 키 |
| MQTT port | 컴파일 타임 매크로만 | `/wifi.conf`의 `port` 키 |
| MQTT prefix | 컴파일 타임 매크로만 | `/wifi.conf`의 `topic_prefix` 키 |
| 노드 nickname | (미지원) | `/node.conf`의 `nickname` 키 |

**장점**:
- 사람이 읽기 쉬운 텍스트 포맷 (cat /wifi.conf 가능)
- 모든 설정이 한 곳에 → 백업/마이그레이션 용이
- NVS는 자체 키/피어 영속화에만 전용 → 관심사 분리
- nickname → BLE 광고 이름 자동 반영 (`bitchat-<nick>` 또는 `bitchat`)

**자동 마이그레이션** (1회):
- 부팅 시 SPIFFS에 `/wifi.conf`가 없고 NVS에 ssid가 있으면 자동 이관
- 이관 후 NVS에서 ssid/pass 삭제 → 다음 부팅부터 SPIFFS-only

### nickname 활용

```text
> node nick MyRelay
> node
=== node profile ===
self_id=8a3f2c1b...
nickname=MyRelay
ble_adv_name=bitchat-<nick>
(file: /node.conf)

[재부팅 후]
BLE advertising started as bitchat-MyRelay (NimBLE)
```

- BLE 스캐너가 다른 bitchat 노드와 구분 가능
- 여러 릴레이 운영 시 식별 용이 (예: `bitchat-LivingRoom`, `bitchat-Balcony`)
- 빈 nickname이면 기본값 `bitchat` (기존 동작 유지)

### 플래시 수명

- 설정 파일 변경 시에만 SPIFFS write (Serial 명령 입력 시)
- 일반 사용자: 설정 1-2회/월 변경 → 사실상 마모 무시
- `/node.conf` ~50B, `/wifi.conf` ~150B → SPIFFS 1MB 중 0.02% 사용

---

### 플래시 파티션 레이아웃 (4MB)

```
Offset    Size      Partition      용도
0x000000  ~32KB     bootloader     1st stage bootloader
0x008000  4KB       partition tbl  파티션 테이블
0x009000  24KB      nvs            자체 키/피어/카운터 + bat_ignore 플래그 (storage.cpp)
0x00F000  (padding)
0x010000  2,848KB   app0           펌웨어 (현재 ~1.2MB, 42% 사용)
0x2F0000  1,024KB   spiffs         블랙리스트 9KB + 1,015KB 여유 (메시지 캐시/설정)
0x3F0000  64KB      coredump       크래시 분석
```

OTA 파티션(app1) 제거 + SPIFFS 1MB 확장. app0는 default.csv의 1,280KB 대비 2.22배.

---

## 배터리 무시 명령 (bat ignore)

배터리 전압이 임계치(3.40V LOW, 3.10V CRIT) 이하로 떨어질 때 자동으로 취해지는 액션(BLE 중지, 딥슬립)을 강제로 무시하는 플래그.

### Serial 명령

```text
bat                    # 현재 전압 + ignore 플래그 상태 출력
bat status             # 위와 동일 (alias)
bat ignore            # ignore 플래그 상태만 출력
bat ignore on         # 강제 BLE keep-alive (CRIT 도달해도 중지 안 함, NVS 영속화)
bat ignore off        # 정상 동작으로 복귀 (NVS 영속화)
bat read              # ADC에서 전압 1회 즉시 읽기
```

### 작동 방식

1. **플래그 저장**: NVS namespace `bchat`의 `bat_ign` 키에 `uint8_t` (0/1)로 저장
2. **재부팅 시 자동 적용**: `power_is_battery_ignored()`가 boot 시 NVS에서 lazy-load → `power_handle_idle()`의 배터리 측정 루틴에서 조회
3. **캐시 무효화**: `bat ignore on/off` 실행 즉시 `power_reload_battery_ignore()` 호출 → 다음 번 측정부터 새 값 적용
4. **영향 받는 동작**:
   - `power.cpp`: V_CRIT 도달 시 "BLE 중지 + 딥슬립" 액션 스킵 (현재 TODO 라인)
   - V_LOW 경고 로그는 그대로 출력 (단 "IGNORED by flag" 접미사 추가)

### 사용 사례

- **USB 상시 전원 환경**에서 배터리 분배 핀이 있어 가짜 CRIT 전압이 읽히는 경우 → `bat ignore on`
- **Heltec WiFi LoRa 32 V2**에서 GPIO 13 (ADC2)이 WiFi bridge 켜지면 garbage 값 읽어 CRIT 오탐지 → `bat ignore on`으로 회피 (BLE-only 운영에서는 정상 측정 가능하므로 off 유지 권장)
- **TTGO T-Beam**은 AXP192 PMU 경유라 ADC disabled → bat_ignore 플래그 의미 없음
- **배터리 없는 DevKitC**에서도 ADC pin이 floating 상태면 잘못된 값 읽힐 수 있으므로 `bat ignore on` 권장

---

## 딥슬립 제어 (sleep deep)

ESP32 딥슬립은 wake 시 무조건 reset이 발생합니다 (RAM 초기화, setup()부터 재실행). BLE 메시 릴레이 노드에서는 이 동작이 문제가 될 수 있습니다:

- **BLE 연결 끊김**: 모든 peer 연결 해제, 재연결 필요
- **RAM 상태 손실**: peer_keys, msg_cache, retry_queue 등 휘발성 데이터 손실
- **~1초 재부팅**: 그 동안 들어오는 메시지 유실 가능

### 기본값: OFF (light sleep만 사용)

`esp_pm_configure(light_sleep_enable=true)`가 이미 활성화되어 있어, **CPU idle 시 자동으로 light sleep**에 진입합니다:
- CPU 클럭 다운, RAM 상태 보존, BLE 컨트롤러 계속 동작
- 패킷 수신 시 즉시 wake (지연 없음)
- 배터리 소모: ~5-10mA (BLE advertising 1.28s 간격)

### Serial 명령

```text
sleep             # 절전 모드 상태 출력 (battery_ignore, spiffs_cache, deep_sleep)
sleep deep on     # 5분 idle 시 딥슬립 허용 (NVS 영속화, 극단 배터리 절약)
sleep deep off    # 딥슬립 비활성화 (기본값, NVS 영속화)
```

### 동작 비교

| 항목 | deep sleep OFF (기본) | deep sleep ON |
|------|----------------------|---------------|
| 5분 idle 시 동작 | auto light sleep 유지 | 60초 딥슬립 → wake reset |
| BLE 연결 | 유지 (peer 재연결 불필요) | 끊김 (peer 재연결 필요) |
| RAM 상태 | 보존 | 손실 (peer_keys, msg_cache, retry_queue) |
| 전력 소모 (idle) | ~5-10mA | ~0.5mA (거의 0) |
| Wake 지연 | 0ms (즉시) | ~1000ms (재부팅) |
| 추천 용도 | BLE 메시 릴레이 노드 | 가끔 깨어나는 센서 노드 |

### 사용 사례

- **BLE 메시 릴레이 노드**: 기본 OFF 유지 → BLE continuity + RAM 보존 우선
- **장기 미사용 / 배터리 극단 절약 필요**: `sleep deep on` → 5분 idle 시 60초 딥슬립
- **배터리 부족 시 긴급 절전**: ON 후 재부팅 시 USB 충전 권장

### ESP32 딥슬립의 정상 동작 (주의)

로그에 아래와 같이 나타나면 정상입니다:
```
[W] idle 300001 ms > threshold 300000, deep_sleep=ON → entering deep sleep
[I] entering deep sleep for 60000000 us...
ets Jun  8 2016 00:22:57   ← 60초 후 자동 wake → reset 발생
rst:0x5 (DEEPSLEEP_RESET), boot:0x13 (SPI_FAST_FLASH_BOOT)
...
=============================
 bitchat ESP32 relay firmware
 build: ...
=============================      ← setup()부터 재실행
```

이 동작을 원치 않으면 `sleep deep off` (기본값)로 두면 됩니다.

---

## peer_keys 매핑 (SPIFFS 영속화)

`src/peer_keys.h/.cpp` - senderID (8B) → Ed25519 공개키 (32B) 매핑:

- bitchat 패킷의 senderID는 8B지만 서명 검증에는 32B pk 필요
- ANNOUNCE 메시지 수신 시 자동으로 매핑 추가 (ble_relay.cpp)
- MESSAGE 수신 시 매핑 조회하여 서명 검증

### 백엔드: NVS → SPIFFS 이관

이전에는 NVS blob (`bpkey` namespace)에 저장했으나, SPIFFS로 이관:

| 항목 | 이전 (NVS) | 현재 (SPIFFS) |
|------|-----------|----------------|
| 파일/namespace | `bpkey/keys` blob | `/peer_keys.bin` 파일 |
| 용량 | ~2.4KB blob | 9,472B (128개 × 74B) |
| 부팅 시 동작 | NVS blob 로드 | SPIFFS 로드, NVS 마이그레이션(1회) |
| 장점 | (이전) | 다른 설정(node.conf, wifi.conf)과 일관된 관리 |

**자동 마이그레이션** (1회):
- 부팅 시 `/peer_keys.bin`이 없고 NVS에 `bpkey` blob이 있으면 → SPIFFS로 이관 → NVS에서 삭제
- 이후 모든 동작은 SPIFFS-only

### Serial 명령

```text
peer              # 현재 매핑 dump (alias of list)
peer list         - 모든 매핑 출력 (senderID + pk + key_mask)
peer count        - 매핑 개수만 출력
peer clear        - 전체 삭제 (SPIFFS /peer_keys.bin 제거)
```

매핑 추가는 자동: BLE ANNOUNCE 메시지 수신 시 자동 추가됩니다. 수동 add는 Serial로 제공하지 않습니다.

---

## mTLS 인증서 기반 보안 MQTT (port 8883)

기존 평문 MQTT (port 1883) 대신 mTLS로 브로커와 상호 인증하는 보안 연결을 지원합니다.

> **자체 CA 구축 가이드**: `tools/README.md` 참조 (CA 생성, 클라이언트 인증서 발급, 만료 관리, 회전 절차)
>
> **빠른 시작**:
> ```bash
> cd tools/
> ./gen_ca.sh                              # 자체 CA 생성 (1회)
> ./gen_client_cert.sh esp32-relay-1 365   # 클라이언트 인증서 발급 (1년)
> # broker_ca.pem, client_cert.pem, client_key.pem → ESP32에 업로드
> # Serial: cert upload ca/client/key, wifi tls on, wifi on
> ```

### 인증서 파일 (SPIFFS)

| 파일 | 용도 | 필요 여부 |
|------|------|-----------|
| `/broker_ca.pem` | 브로커 CA 인증서 (브로커 검증) | **TLS 시 필수** |
| `/client_cert.pem` | 클라이언트 인증서 (브로커가 클라이언트 검증) | mTLS 시 필수 |
| `/client_key.pem` | 클라이언트 개인키 | mTLS 시 필수 |

### Serial 명령

```text
# 인증서 관리
cert                       # cert 파일 상태 출력
cert status                - 위와 동일
cert upload <ca|client|key> - PEM 내용 붙여넣기 (EOF 라인으로 종료)
cert clear                 - 모든 PEM 파일 삭제
cert show <ca|client|key>  - PEM 파일 내용 출력 (디버깅)

# TLS 활성화
wifi tls                   - 현재 TLS 상태 + 인증서 존재 여부
wifi tls on                - mTLS 활성화 (port 자동 8883)
wifi tls off               - 비활성화 (기본값, port 1883 평문)
```

### PEM 업로드 절차 (예시)

```
> cert upload ca
cert: paste PEM content for /broker_ca.pem
cert: end with a single line containing only 'EOF' (or send empty line)
-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIUTLrM...
(PEM 내용 붙여넣기)
-----END CERTIFICATE-----
EOF
cert: upload done (1234 bytes, 22 lines) → /broker_ca.pem

> cert upload client
... 동일하게 client cert PEM 붙여넣기, EOF로 종료 ...

> cert upload key
... 동일하게 client private key PEM 붙여넣기, EOF로 종료 ...

> cert status
=== cert status ===
  /broker_ca.pem   : OK
  /client_cert.pem : OK
  /client_key.pem  : OK
  mask=0x7 (mTLS ready mode)
  use_tls=OFF (current wifi.conf)

> wifi tls on
wifi: tls → ON (SPIFFS, ok=1, port=8883)
wifi: 변경 적용하려면 'wifi off' 후 'wifi on' 재실행

> wifi on
wifi_bridge: ready (broker=broker.example.com:8883, tls=ON, ...)
wifi_bridge: loaded broker CA from /broker_ca.pem (1234 bytes)
wifi_bridge: loaded client cert from /client_cert.pem (1456 bytes)
wifi_bridge: loaded client key from /client_key.pem (1704 bytes)
wifi_bridge: mTLS enabled (CA + cert=loaded + key=loaded)
mqtt: connecting to broker broker.example.com:8883 ...
mqtt: connected as bchat-xxxx
```

### 동작 모드

| mask | 인증서 상태 | TLS 동작 |
|------|-------------|----------|
| 0 (none) | 파일 없음 | TLS 활성화 시도 → 자동 fallback 평문 |
| 1 (CA only) | CA만 | CA-only TLS (서버만 검증, 클라이언트 인증 생략) |
| 3 (CA+cert) | CA + client cert | 클라이언트 인증 불완전 → mTLS 연결 실패 가능 |
| 7 (CA+cert+key) | 전부 있음 | **mTLS 정상 동작** (권장) |

### 구현 상세

- `WiFiClientSecure` + `loadCACert()` + `loadCertificate()` + `loadPrivateKey()`
- PubSubClient에 secure client 주입: `g_mqtt_client.setClient(g_wifi_secure)`
- 부팅 시 `/wifi.conf`의 `use_tls=1`이면 자동으로 인증서 로드 + port 8883
- 인증서 없으면 자동으로 평문 MQTT (port 1883)로 fallback + 경고 로그

### 보안 고려사항

- **CA 인증서**: 공개되어도 무방 (브로커 검증용)
- **client cert**: 공개되어도 무방 (브로커가 검증하는 공개 정보)
- **client private key**: 절대 비공개 → SPIFFS에만 저장, Serial로 dump 안 함
  - 단, `cert show key` 명령은 디버깅용으로 출력 (운영 환경에서는 비활성화 권장)
- TLS handshake 시 메모리: ~40KB 추가 사용 (mbedTLS handshake buffer)
- 암호화된 통신: TLS 1.2 (arduino-esp32 v2.0.17 기본), AES-128-GCM 또는 AES-256-GCM

### 인증서 회전 (rotation)

클라이언트 인증서는 유효기간(기본 1년)이 있어 만료 전에 갱신해야 합니다.

**만료 모니터링** (cron 추천):
```bash
# 매일 09:00 검사, 30일 이내 만료 시 알림
0 9 * * * /path/to/tools/check_cert_expiry.sh /path/to/esp32-relay.crt 30 \
    || mail -s "ESP32 cert expiring soon" admin@example.com < /dev/null
```

**회전 절차**:
```bash
cd tools/
./rotate_certs.sh esp32-relay-livingroom 365
#   - 기존 인증서 .bak.YYYYMMDD-HHMMSS 확장자로 자동 백업
#   - 새 인증서 발급 (CA 서명)
#   - broker_ca.pem, client_cert.pem, client_key.pem 갱신
#   - ESP32 업로드 절차 안내 + 롤백 지시 출력
```

상세 절차는 `tools/README.md`의 "인증서 만료 관리" 섹션 참조.

---

## ECDH 키 교환 + AES-GCM payload 암호화 (가능성 분석)

mTLS로 MQTT 브로커 연결 보안은 확보했지만, BLE 메시 네트워크 내에서는 패킷이 평문으로 전송됩니다 (BLE 자체 암호화는 페어링 시에만). 릴레이 노드 간 end-to-end 암호화를 위한 ECDH + AES-GCM 도입 가능성을 분석합니다.

### 기술적 가용성: ✅ 가능

| 항목 | 라이브러리 | 상태 |
|------|-----------|------|
| ECDH (X25519) | libsodium `crypto_scalarmult_curve25519` | 이미 libsodium 사용 중 (crypto.cpp) |
| ECDH (mbedTLS) | `mbedtls_ecdh_*`, Curve25519 지원 | arduino-esp32 v2.0.17에 포함 |
| AES-GCM | mbedTLS `mbedtls_gcm_*` | arduino-esp32 v2.0.17에 포함 |
| AEAD 대안 | libsodium `crypto_aead_xchacha20poly1305` | libsodium 자체 지원 |

### 메모리/플래시 영향 추정

| 항목 | 추가 크기 | 현재 상태 |
|------|----------|-----------|
| mbedTLS ECDH 코드 | ~10KB | app0 49.3% → ~51% (여전히 여유) |
| mbedTLS AES-GCM 코드 | ~5KB | |
| ECDH 세션당 RAM | ~200B (privkey + pubkey + shared secret) | 32개 peer × 200B = 6.4KB RAM |
| AES-GCM nonce | 12B per packet | 기존 패킷당 12B 오버헤드 |

### 구현 복잡도 (높음)

이 작업은 단순 API 추가가 아니라 패킷 포맷 + 프로토콜 변경을 수반:

1. **키 교환 단계**:
   - 패킷 헤더에 X25519 공개키 (32B) 포함하는 새 ANNOUNCE 확장
   - 상대방의 X25519 공개키 수신 시 `crypto_scalarmult_curve25519`로 shared secret 도출
   - peer_keys 모듈에 shared_secret 필드 추가 (32B × 32개 = 1KB)

2. **세션 관리**:
   - 각 peer마다 독립적인 shared secret + nonce counter
   - peer_keys.cpp 확장: peer_keys_get_session() API 추가
   - SPIFFS에 영속화 필요 (세션 키는 reboot 후 유지되어야 함)

3. **패킷 암호화**:
   - bitchat_protocol.cpp 확장: 패킷 헤더에 encrypted flag + nonce + tag 추가
   - payload만 암호화 (헤더는 평문 - relay가 TTL 감소 등 처리 가능)
   - AES-GCM: 96-bit nonce + 128-bit tag

4. **하위 호환성**:
   - 기존 평문 패킷과 새 암호화 패킷이 공존해야 함
   - 패킷 헤더의 새로운 flag로 구분 (예: `flags & 0x80 = encrypted`)
   - 릴레이는 암호화된 패킷을 그대로 forward (payload만 암호화되므로)

5. **API 변경 영향**:
   - ble_relay.cpp: 암호화된 패킷 수신 시 복호화 → 서명 검증 → 처리
   - lora_relay.cpp: LoRa도 동일한 암호화 적용 (양쪽 모두 같은 shared secret)
   - wifi_bridge.cpp: MQTT payload도 암호화 (이미 mTLS로 전송 보안은 확보되었으나 end-to-end는 추가)

### 권장 접근법

**1단계 (현재 완료)**: mTLS로 MQTT 브로커 연결 보안 확보 (전송 구간 암호화)
**2단계 (향후 작업)**: BLE 메시 end-to-end 암호화 도입
   - libsodium의 `crypto_aead_xchacha20poly1305` 우선 검토 (mbedTLS보다 단순 + libsodium 이미 사용 중)
   - X25519 ECDH로 shared secret 도출 → HKDF로 키 파생 → AEAD 암호화
   - 점진적 마이그레이션: 새로운 패킷 타입 추가 (기존 패킷과 공존)

### 결론

- **기술적 가능성**: 충분히 가능 (라이브러리, 메모리, CPU 모두 OK)
- **구현 범위**: 큰 작업 (패킷 포맷 + 프로토콜 + 세션 관리 + 하위 호환성)
- **권장 순서**: mTLS 먼저 적용 (완료) → BLE end-to-end 암호화는 별도 이터레이션으로 진행
- **대안**: BLE pairing (LE Secure Connections)으로 링크 단위 암호화도 가능하지만, 모든 peer와 페어링해야 하는 제약

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

### 빌드 크기 비교 (NimBLE + LoRa + WiFi + SPIFFS 블랙리스트 + bat_ignore 포함, 배터리 모드)

> 파티션 테이블 `partitions/no_ota_spiffs_1mb.csv` 적용 기준 (app0=2,848KB, SPIFFS=1,024KB)

| 보드 | RAM (static) | Flash | app0 사용률 | 비고 |
|------|-------------|-------|-------------|------|
| ESP32 클래식 | 37.0% (121KB) | 1,405KB | **49.3%** (app0 2,848KB) | mTLS + 128 peer_keys (heap 9.5KB) |
| ESP32-C3 | 33-34% (~110KB) | 1,384KB | **48.6%** (app0 2,848KB) | 동일 |
| ESP32-S3 | 36% (~119KB) | 1,292KB | **45.3%** (app0 2,848KB) | 가장 여유 (8MB Flash) |

> peer_keys 128개 엔트리는 heap 동적 할당 (calloc)으로 처리하여 정적 .bss 부담 0.
> 부팅 시 9.5KB heap 사용 → ESP32 heap 200+KB 중 5% 미만.

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

## 큰 패킷 분할/재조립 (packet_splitter)

`src/packet_splitter.h/.cpp`에 LoRa 프레임 한계 222B로 인해 큰 bitchat 패킷(최대 623B)이 drop되는 문제를 해결하기 위한 자동 분할/재조립 모듈.

### 동작 원리

큰 패킷(>222B)이 LoRa로 송신될 때 자동으로 분할되어 여러 청크로 전송되고, 수신 측에서 청크를 모아 원본 패킷을 복원합니다.

**청크 포맷** (14B header + payload, 최대 222B = 헤더 14B + 페이로드 208B):

```
+-------+------+----------+----------+-----------+
| magic | ver  | total    | index    | hash(8B)   |
| 0xCF  | 0x01 | chunks   | (0-based)| (sender_pk  |
|       |      | (1..16)  |          | [0..7] XOR  |
|       |      |          |          | counter BE)  |
+-------+------+----------+----------+-----------+
| payload_len (2B)                                  |
+----------------------------------------------------+
| payload (≤208 bytes)                               |
+----------------------------------------------------+
```

- **magic 0xCF**: bitchat 원본 패킷 0xBC와 구분
- **hash**: 원본 패킷 식별용 8B 해시 (sender_pk[0..7] XOR counter[0..7] big-endian)
- **total_chunks**: 총 청크 수 (최대 16)
- **chunk_index**: 현재 청크 인덱스 (0-based)
- **payload_len**: 이 청크의 페이로드 길이

### 분할 시나리오

| 원본 패킷 크기 | 청크 수 | 총 전송량 (오버헤드 14B × 청크 수) |
|----------------|---------|----------------------------------|
| ≤ 222B (작은 패킷) | 1 (분할 없음) | +0B |
| 222B < N ≤ 430B | 2 | +28B (6.5%) |
| 430B < N ≤ 638B | 3 | +42B (7%) |
| 623B (최대) | 3 | +42B (7%) |

### 송신 흐름

```
[BLE 패킷 수신 (큰 패킷)]
       │
       ▼
  forward_to_others
       │
       ▼
  lora_relay_broadcast (len > 222?)
       │
       ├── Yes → splitter_send_chunks
       │           ├── chunk 0/3: [0xCF|0x01|3|0|hash|len][payload_part_0] → LoRa TX
       │           ├── chunk 1/3: [0xCF|0x01|3|1|hash|len][payload_part_1] → LoRa TX
       │           └── chunk 2/3: [0xCF|0x01|3|2|hash|len][payload_part_2] → LoRa TX
       │
       └── No → LoRa.beginPacket / write / endPacket (그대로)
```

### 수신 흐름

```
[LoRa 패킷 수신]
       │
       ▼
  lora_relay_handle_receive
       │
       ├── magic 0xCF (chunk) → splitter_receive_chunk
       │                          │
       │                          ├── 같은 hash 가진 슬롯 찾기
       │                          ├── 비트맵에 chunk_index 기록
       │                          ├── 모든 청크 도착?
       │                          │   ├── No → 대기 (내부 버퍼에 저장)
       │                          │   └── Yes → 원본 패킷 복원 → BLE 큐로 enqueue
       │                          │
       │                          └── 30초 타임아웃 시 슬롯 drop
       │
       └── magic 0xBC (bitchat) → BLE 큐에 바로 enqueue
```

### 재조립 슬롯

- **16개 동시 슬롯**: 16개의 서로 다른 분할 패킷을 동시에 재조립 가능
- **메모리 사용량**: 16 × 623B + α = 약 10KB RAM
- **타임아웃**: 30초 (한 청크라도 30초 내에 안 오면 슬롯 drop)
- **LRU 정책**: 16개 슬롯 꽉 차면 가장 오래된 슬롯 evict

### Serial 로그 예시

```
[D][bitchat] lora: packet 580 > frame 222, splitting
[D][bitchat] splitter: TX chunk 0/3 (208 bytes payload)
[D][bitchat] splitter: TX chunk 1/3 (208 bytes payload)
[D][bitchat] splitter: TX chunk 2/3 (164 bytes payload)
[I][bitchat] splitter: sent 3 chunks for packet (total_len=580, hash=0x8a3f2c1b)
[I][bitchat] lora: split TX complete (580 bytes → chunks)

[D][bitchat] lora: RX 222 bytes rssi=-65 snr=8.5 (total=18)
[D][bitchat] splitter: chunk 0/3 stored, mask=0x01 (need 0x07)
[D][bitchat] lora: RX 222 bytes rssi=-67 snr=8.0 (total=19)
[D][bitchat] splitter: chunk 1/3 stored, mask=0x03 (need 0x07)
[D][bitchat] lora: RX 178 bytes rssi=-62 snr=9.5 (total=20)
[I][bitchat] splitter: ✓ reassembled 3 chunks → 580 bytes (hash=0x8a3f2c1b)
[I][bitchat] lora: reassembled packet 580 bytes → BLE queue
```

### 한계 및 설정값

| 항목 | 기본값 | 비고 |
|------|--------|------|
| `BC_MAX_CHUNKS_PER_PACKET` | 16 | 최대 청크 수 = 원본 623B / 208B = 3개 필요 |
| `BC_CHUNK_PAYLOAD_MAX` | 208 | LoRa 프레임 222B - 헤더 14B |
| `BC_CHUNK_TIMEOUT_MS` | 30000 | 30초 내 미완료 시 슬롯 drop |
| 동시 재조립 슬롯 | 16 | 16 × 623B = 10KB RAM 추가 |

### 한계

- **청크 순서 보장 안 함**: 먼저 온 청크부터 처리, 비트맵으로 추적
- **BLE에는 적용 안 됨**: BLE는 NimBLE가 자체 MTU fragmentation 처리 (247B→분할 자동)
- **MQTT에는 적용 안 됨**: PubSubClient가 buffer 크기 내에서 자동 처리 (BC_MAX_PKT_LEN × 2)
- **단일 링크 한계**: 다른 relay를 거치면 분할/재조립이 링크마다 독립적으로 동작


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
- ~~ESP32 클래식 Flash 92.2%~~ → **해결**: `partitions/no_ota_spiffs_1mb.csv` 적용으로 app0 2,848KB 확보, 현재 사용률 43.8% (SPIFFS 1MB 추가)

---

## 파일 구조

```
bitchat-esp32-relay/
├── README.md                      # 이 파일
├── platformio.ini                 # PlatformIO 빌드 (9 envs: esp32dev/c3/s3 + USB 변형 + heltec_v2/t-beam)
├── partitions/                    # 사용자 정의 파티션 테이블
│   ├── no_ota_spiffs_1mb.csv      # OTA 제거 + app0 2.8MB + SPIFFS 1MB (현재 사용)
│   └── no_ota_max_app_spiffs.csv  # OTA 제거 + app0 3.6MB + SPIFFS 128KB (백업)
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
│   ├── blacklist.h/.cpp           # pk(256)/BLE(128) 블랙리스트 (SPIFFS 영속화)
│   ├── lora_relay.h/.cpp          # LoRa 자동감지 + BLE↔LoRa 브릿지 (SX127x)
│   ├── packet_splitter.h/.cpp     # 큰 패킷 분할/재조립 (LoRa 222B 한계 우회)
│   ├── msg_cache.h/.cpp          # store-and-forward RAM 캐시 (32 entries, 12h TTL)
│   ├── msg_store.h/.cpp          # SPIFFS overflow backend (3072 entries, /msg_store.bin)
│   ├── node_config.h/.cpp        # SPIFFS config files (/node.conf, /wifi.conf)
│   ├── peer_keys.h/.cpp          # senderID → Ed25519 pk 매핑 (128 entries, SPIFFS /peer_keys.bin, heap allocated)
│   ├── wifi_bridge.h/.cpp         # WiFi+MQTT 브릿지 (BLE↔인터넷, Serial on/off)
│   └── ble_relay.h/.cpp           # BLE GATT + relay (NimBLE, 배터리 모드 시 저듀티)
├── lib/                           # 외부 라이브러리 (자체 포함)
│   ├── NimBLE-Arduino/            # h2zero NimBLE-Arduino 1.4.2
│   ├── LoRa/                      # Sandeep Mistry arduino-LoRa 0.8.0
│   └── PubSubClient/              # knolleary PubSubClient 2.8 (MQTT)
├── tools/                         # mTLS 인증서 관리 도구
│   ├── README.md                  # CA 구축/발급/회전 가이드
│   ├── openssl.cnf                # OpenSSL 설정 (CA/client/server 확장)
│   ├── gen_ca.sh                  # 자체 CA 생성 (ca.key, ca.crt, ca.srl)
│   ├── gen_client_cert.sh         # 클라이언트 인증서 발급 (CA 서명)
│   ├── check_cert_expiry.sh       # 만료일 확인 (cron 자동화 가능)
│   └── rotate_certs.sh            # 인증서 회전 (백업 + 새 발급)
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

- ~~ESP32 클래식 Flash 92.2%~~ → **해결**: OTA 제거 + SPIFFS 1MB 확장 파티션 적용으로 app0 2,848KB (43.8% 사용), SPIFFS 1MB 추가
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
