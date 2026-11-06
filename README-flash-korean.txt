================================================================
bitchat ESP32 relay v30 - ADC/SPI/외부전원 가이드
================================================================

【v30 추가 수정 (배터리 ADC + SPI + 외부 전원 회로 명시)】

  A. 배터리 ADC 핀 활성화 (사용자 지정):
     - C3 Super Mini: GPIO0 (ADC1_CH0)
     - S3 Super Mini: GPIO4 (ADC1_CH3)
     - ESP32 클래식: GPIO35 (ADC1_CH7, 기존 유지)
     - 전압 분배 회로 2:1 필수 (LiPo 4.2V → ADC 2.1V)

  B. C3 SPI 핀아웃 추가 (외부 LoRa 모듈 연결용):
     ┌────────────────────────────────────────────┐
     │  C3 Super Mini SPI/LoRa 핀아웃              │
     ├──────────┬──────────┬───────────────────────┤
     │ GPIO     │ 기능      │ 비고                  │
     ├──────────┼──────────┼───────────────────────┤
     │ GPIO 4   │ SCK      │ SPI 클럭              │
     │ GPIO 5   │ MISO     │ SPI MISO (자동 할당)  │
     │ GPIO 6   │ MOSI     │ SPI MOSI              │
     │ GPIO 20  │ CS       │ 칩 셀렉트             │
     │ GPIO 2   │ RESET    │ 리셋                  │
     │ GPIO 1   │ IRQ/BUSY │ SX127x=DIO0/SX126x=BUSY│
     │ GPIO 3   │ DC       │ SX126x 전용 (SX127x 미사용)│
     │ GPIO 0   │ 배터리 ADC │ ADC1_CH0            │
     └──────────┴──────────┴───────────────────────┘
     - SX127x (LoRa library 호환): CS=20, RST=2, IRQ=1, DC 미사용
     - SX126x (별도 라이브러리 필요): CS=20, RST=2, DC=3, BUSY=1
     - LoRa 모듈 미연결 시 자동으로 BLE-only 모드로 동작

  C. ★★★ 외부 전원 회로 필수 ★★★
     C3/S3 Super Mini의 온보드 3.3V 레귤레이터는 LoRa TX 시
     부족한 전류를 공급하지 못합니다 (LoRa TX 20dBm = ~120mA).

     외부 전원 회로 권장 구성:
     ┌──────────┐     ┌──────────────┐     ┌──────────┐
     │ 배터리    │────→│ DC-DC Buck   │────→│ 3.3V      │──→ LoRa 모듈 VCC
     │ (LiPo)   │     │ (MP1584 등)  │     │ (독립)    │
     └──────────┘     └──────────────┘     └──────────┘
          │                                        │
          └──── C3/S3 Super Mini USB 또는 별도 전원  │
                                                   │
     공통 GND 필수 (C3/S3와 외부 전원 회로의 GND 연결)

     권장 사양:
     - DC-DC Buck 컨버터: MP1584 / LM2596 / TPS5430
     - 출력: 3.3V, 500mA 이상 (LoRa TX 피크 대응)
     - 입력: LiPo 3.7V 또는 5V USB
     - LoRa 모듈 전원은 C3/S3 3.3V 핀이 아닌 외부 레귤레이터에서 공급

【v29 발열/시리얼 리셋 해결 (유지)】
  - DFS: CPU 유휴 시 80→40MHz 자동 강하 (C3), 160→80MHz (S3)
  - 시리얼 종료 리셋 방지: S3 enableReboot(false), C3 monitor_rts/dtr=0
  - BLE 주기: 광고 100-200ms, 스캔 1024ms/30ms (듀티 2.9%)

【v29 추가 수정 (발열 + 시리얼 종료 리셋 해결)】
  v28에서 절전 모드를 완전히 꺼서 발열이 심해지고,
  시리얼 모니터 종료 시 DTR 드롭으로 리셋되는 현상 발생.

  v29 수정:
    A. 시리얼 종료 리셋 방지:
       - S3 (USBCDC/TinyUSB): Serial.enableReboot(false) 추가
         → DTR/RTS 4단계 시퀀스 무시, 종료 시 리셋 안 됨
       - C3 (HWCDC): enableReboot() 메서드 없음
         → platformio.ini에 monitor_rts=0, monitor_dtr=0 추가
         → PlatformIO 모니터가 DTR/RTS 토글 안 함
       - 대체: screen /dev/ttyACM0 115200 (DTR/RTS 토글 없음)

    B. DFS (Dynamic Frequency Scaling) 재활성화:
       - v28: CONFIG_PM_ENABLE=n (절전 완전 off → 발열 심함)
       - v29: CONFIG_PM_ENABLE=y + light_sleep_enable=false
         → CPU 유휴 시 자동으로 클럭 강하 (발열 대폭 감소)
         → C3: 80→40MHz, S3: 160→80MHz 자동 강하
       - power_init()에서 USB 모드도 DFS 적용하도록 수정
         (이전: USB 모드는 PM 설정 스킵 → v29: 항상 DFS 적용)

    C. CPU 클럭 강하:
       - C3: f_cpu 160MHz → 80MHz (50% 감소, 발열 대폭 감소)
       - S3: DFS max_freq 240→160MHz (자동 강하)
       - BLE/USB CDC 모두 80MHz에서 정상 동작

    D. BLE 주기 느리게 조정 (발열 감소):
       - 광고 간격: 3.75-11.25ms → 100-200ms (이전의 1/20)
       - 스캔 간격: 80ms/40ms window → 1024ms/30ms window
         (듀티 50% → 2.9%, 라디오 활동 대폭 감소)
       - 메시 릴레이 발견은 1초 이내 가능 → 실용성 유지

【v28 추가 수정 (C3/S3 리셋 루프 해결)】
  v27에서 플래싱 문제는 해결되었으나 C3 Super Mini에서 부팅 후
  BLE advertising 시작 직후 리셋 반복하는 현상이 보고됨.
  부트 로그에서 3가지 문제 발견:
    1. esp_pm_configure failed: 0x0106 (ESP_ERR_NOT_SUPPORTED)
    2. E NimBLEDevice: esp_ble_tx_power_set: rc=-1
    3. LoRa scan이 무효 GPIO(27/23/26) 호출 → 불안정

  v28 수정:
    A. ble_relay.cpp: NimBLEDevice::setPower()를 init() 뒤로 이동
       - 이전: setPower() → init() 순서 (컨트롤러 미초기화 상태에서 호출 → rc=-1)
       - v28:  init() → setPower() 순서 (정상 동작)
       
    B. power.cpp + sdkconfig.defaults.esp32c3: C3 절전 모드 비활성화
       - CONFIG_PM_ENABLE=n (esp_pm_configure 호출 자체를 스킵)
       - light_sleep_enable=false (BLE + light sleep 호환 안 됨)
       - C3 Super Mini는 USB 전원 전용이므로 절전 불필요
    
    C. platformio.ini: C3/S3 모두 BC_POWER_USB_ALWAYS=1 적용
       - 배터리 모드(BC_POWER_BATTERY=1) → USB 상시 모드로 전환
       - 딥슬립/ADC/PM 설정 모두 스킵 → 안정성 대폭 향상
    
    D. lora_relay.cpp + main.cpp: C3에서 LoRa 스캔 스킵
       - C3는 GPIO 27/23/26이 존재하지 않음 (표준 LoRa 핀아웃 무효)
       - BOARD_VARIANT_ESP32C3 정의 시 LoRa 스캔 자동 스킵
       - BC_LORA_FORCE_SCAN=1 빌드 플래그로 강제 스캔 가능
    
    E. power.h: C3/S3 Super Mini 배터리 ADC 비활성 (GPIO 35 C3에 없음)
       - BC_BATTERY_ADC_PIN=0xFF (C3/S3), =35 (ESP32 클래식)
    
    F. power.h: S3 Super Mini LED 핀 GPIO 48로 수정 (이전 5 → 48)

【v27 결정적 플래시 설정 (유지)】
  - sdkconfig.defaults에 CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
    CONFIG_ESPTOOLPY_FLASHFREQ_40M=y
    CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y 추가
  - bootloader.bin 헤더가 빌드 타임에 4MB/40MHz/DIO로 확정

【v26까지의 문제 요약】
  - ESP32-C3 / ESP32-S3 Super Mini 보드에서 플래싱 후 부팅 실패
  - 증상: "waiting for download" 무한 대기 / SHA mismatch / 시리얼 출력 없음
  - 근본 원인: bootloader.bin 헤더가 1MB/26MHz로 잘못 생성됨
    (firmware.bin은 4MB/40MHz로 정상이나 bootloader만 잘못됨)

【원인 상세】
  ESP32 이미지 헤더의 처음 4바이트:
    byte 0: 0xE9 (매직)
    byte 1: 세그먼트 수
    byte 2: SPI 모드 (0x02=DIO, 0x03=DOUT, ...)
    byte 3: (high nibble) 플래시 크기 + (low nibble) 주파수
            0x20 = 4MB / 40MHz (정상)
            0x02 = 1MB / 26MHz (v26의 잘못된 상태)

  board JSON의 f_flash/flash_mode는 "firmware(app) 이미지"에만 적용되고
  bootloader는 sdkconfig의 CONFIG_ESPTOOLPY_* 설정을 따름.
  v26 sdkconfig.defaults에 이 설정이 누락되어 bootloader가 기본값(1MB/26MHz)
  으로 빌드된 것이 근본 원인.

【v27 결정적 수정】
  1. sdkconfig.defaults에 다음 3줄 추가 (모든 보드 공통):
       CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
       CONFIG_ESPTOOLPY_FLASHFREQ_40M=y
       CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
       CONFIG_ESPTOOLPY_FLASHSIZE="4MB"

  2. sdkconfig.defaults.esp32c3 / .esp32s3 파일에 동일 설정 명시
     (타겟별 오버라이드 보장)

  3. S3 전용 sdkconfig.defaults.esp32s3 신규 작성:
       - 플래시 설정 (4MB/40MHz/DIO)
       - PSRAM Quad 80MHz (FH4R2 2MB PSRAM)
       - USB CDC (CONFIG_TINYUSB_CDC_ENABLED=y)
       - RSA 2048 + X509 cert write (cert gen용)

  4. platformio.ini esp32s3 env 수정:
       - board_build.sdkconfig = sdkconfig.defaults.cdc 제거
         (이 설정이 sdkconfig.defaults.esp32s3 로딩을 방해했음)
       - board_build.f_flash = 40000000L 명시적 추가

  5. sdkconfig.defaults.cdc 파일 삭제 (설정이 .esp32c3/.esp32s3로 이동)

【검증 결과】
  $ python3 tools/verify_headers.py
  모든 보드의 bootloader.bin / firmware.bin / merged-flash.bin 헤더가
  DIO / 4MB / 40MHz (byte2=0x02, byte3=0x20) 로 정상 생성됨.

================================================================
플래싱 방법 (2가지 중 택1)
================================================================

【방법 A - 통합 스크립트 사용 (권장)】

  1단계: 사용 가능한 시리얼 포트 확인
    python3 tools/flash_bitchat.py --list

    Super Mini 보드가 /dev/ttyACM0 또는 COM3 등으로 나타나야 함.
    안 보이면:
      - 데이터 통신 지원 USB-C 케이블인지 확인 (충전 전용은 안 됨)
      - BOOT 버튼을 누른 채 USB 꽂기 → ROM 다운로드 모드 진입

  2단계: 플래싱
    # ESP32-C3 Super Mini
    python3 tools/flash_bitchat.py --board esp32c3 --port /dev/ttyACM0

    # ESP32-S3 Super Mini (FH4R2)
    python3 tools/flash_bitchat.py --board esp32s3 --port /dev/ttyACM0

    # ESP32 클래식 (DevKitC 등)
    python3 tools/flash_bitchat.py --board esp32dev --port /dev/ttyUSB0

  3단계 (옵션): 바이너리 검증 포함
    python3 tools/flash_bitchat.py --board esp32c3 --port /dev/ttyACM0 --verify

  4단계 (문제 시): stub 비활성 / 속도 저하 / 전체 삭제
    python3 tools/flash_bitchat.py --board esp32c3 --port /dev/ttyACM0 \
      --no-stub --baud 460800 --erase-all

【방법 B - esptool 직접 사용】

  ESP32 클래식 (bootloader 시작주소 0x1000 주의!):
    esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
      write_flash --flash-mode dio --flash-freq 40m --flash-size 4MB \
      0x1000  firmware/esp32dev/bootloader.bin \
      0x8000  firmware/esp32dev/partitions.bin \
      0x10000 firmware/esp32dev/firmware.bin

  ESP32-C3 Super Mini (bootloader 시작주소 0x0):
    esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
      write_flash --flash-mode dio --flash-freq 40m --flash-size 4MB \
      0x0     firmware/esp32c3/bootloader.bin \
      0x8000  firmware/esp32c3/partitions.bin \
      0x10000 firmware/esp32c3/firmware.bin

  ESP32-S3 Super Mini (FH4R2):
    esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
      write_flash --flash-mode dio --flash-freq 40m --flash-size 4MB \
      0x0     firmware/esp32s3/bootloader.bin \
      0x8000  firmware/esp32s3/partitions.bin \
      0x10000 firmware/esp32s3/firmware.bin

  옵션 - merged-flash.bin 단일 파일로 플래싱 (속도는 느리지만 단순):
    esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
      write_flash --flash-mode dio --flash-freq 40m --flash-size 4MB \
      0x0 firmware/esp32s3/merged-flash.bin

【방법 C - NodeMCU PyFlasher (Windows GUI)】

  NodeMCU PyFlasher는 merged-flash.bin을 0x0 주소에 플래싱 가능.
  - Firmware binary: firmware/esp32c3/merged-flash.bin (또는 esp32s3/...)
  - Flash mode: dio
  - Flash size: 4MB
  - Flash speed: 40MHz
  - 추가 인수: --no-stub  (Super Mini 보드에서 stub 인식 문제 시)

================================================================
플래싱 후 확인
================================================================

  1. EN 버튼을 한 번 누르거나 USB를 뺐다가 다시 꽂아 리셋
  2. 시리얼 모니터 열기 (115200 baud):
       pio device monitor -p /dev/ttyACM0 -b 115200
       또는: screen /dev/ttyACM0 115200
  3. 부팅 메시지 확인:
       "bitchat ESP32 relay v27 starting..."
       "BLE relay: advertising started"
       "Storage: ..." 등

  Super Mini (C3/S3) 참고:
    - 시리얼 출력이 처음 3초간 대기 후 나타납니다 (USB CDC 열거 대기)
    - v26에서 추가된 while(!Serial) 대기 코드가 main.cpp에 있음

================================================================
문제 해결 (Troubleshooting)
================================================================

[증상] 시리얼 포트가 아예 안 보임
  → 데이터 통신 지원 USB-C 케이블인지 확인 (충전 전용 불가)
  → BOOT 버튼 누른 채 USB 꽂기 → ROM 다운로드 모드
  → 다른 USB 포트 / 다른 케이블 시도

[증상] esptool이 "Failed to connect to device" 오류
  → --no-stub 옵션 추가 (Super Mini 일부 보드에서 stub 인식 실패)
  → --baud 460800 으로 속도 낮춤
  → BOOT 버튼 누른 채 전원 인가 → ROM 모드 진입

[증상] 플래싱은 성공하지만 부팅 안 됨
  → 헤더 검증: python3 tools/verify_headers.py
     byte3=0x20이면 정상, 0x02이면 잘못된 빌드
  → --erase-all 옵션으로 전체 삭제 후 재플래싱 (NVS 잔재 제거)

[증상] SHA mismatch / boot loop
  → v26의 merged-flash.bin 수동 헤더 패치는 SHA가 맞지 않음
  → v27에서는 빌드 타임에 헤더 확정 → SHA도 함께 정상
  → 개별 파일 3개 플래싱이 가장 안전

[증상] ESP32-C3 발열로 리셋 반복
  → 40MHz 강제 적용됨을 확인 (펌웨어 헤더 byte3=0x20)
  → 배터리 모드 비활성: --erase-all 후 esp32c3-usb 환경으로 빌드
  → 또는 BC_POWER_USB_ALWAYS=1 빌드

[증상] ESP32-S3 PSRAM 인식 안 됨
  → v27 sdkconfig.defaults.esp32s3에 CONFIG_SPIRAM=y 포함됨
  → 부팅 로그에 "PSRAM initialized" 메시지 확인
  → board JSON에 BOARD_HAS_PSRAM 미포함 시 경고 (무시 가능)

================================================================
빌드 정보
================================================================
  - platform: espressif32@6.7.0 (arduino-esp32 2.0.x)
  - NimBLE-Arduino 1.4.2 (lib/ 포함)
  - 파티션: no_ota_spiffs_1280.csv (app0 2688KB + SPIFFS 1280KB)
  - 빌드 환경:
      pio run -e esp32dev   # ESP32 클래식
      pio run -e esp32c3    # ESP32-C3 Super Mini
      pio run -e esp32s3    # ESP32-S3 Super Mini (FH4R2)

  빌드 후 헤더 자동 검증:
      python3 tools/verify_headers.py
