================================================================
bitchat ESP32 relay - v10 펌웨어 플래싱 가이드
================================================================

v10 수정 내용 (앱 재실행 후 덤프가 안 나오던 버그 수정):
  1. [핵심] 스캔 콜백(onResult)에서 블로킹 connect() 직접 호출 제거
     - onResult는 NimBLE 호스트 태스크에서 호출되는데 거기서 connect()
       하면 호스트 태스크가 자기 완료 이벤트를 기다리는 자기교착 발생
       (매 시도 BLE 전체 4초 블랙아웃 + 방금 맺은 연결 강제 종료)
     - v10: 발견 주소만 큐잉 -> loop() 태스크에서 안전하게 연결
     - 폰이 먼저 서버로 접속하면 outgoing 시도 자동 취소
  2. 광고(advertising) 자가 복구: 클라이언트가 없는데 광고가 꺼져 있으면
     2초마다 자동 재시작 (폰이 릴레이를 못 찾는 상황 원천 차단)
  3. 서버 연결 피어 주소 추적: 이미 서버로 연결된 폰에 outgoing
     2중 연결 시도하지 않음
  4. v9의 accumulator 연결경계 리셋 / resync / 스캔 게이트 /
     3초 연결 타임아웃 모두 유지

플래싱 (esptool):
  [ESP32 classic - esp32dev]  (bootloader = 0x1000 주의!)
    esptool.py --chip esp32 --port <포트> --baud 921600 write_flash \
      0x1000 esp32dev/bootloader.bin \
      0x8000 esp32dev/partitions.bin \
      0x10000 esp32dev/firmware.bin

  [ESP32-C3]
    esptool.py --chip esp32c3 --port <포트> --baud 921600 write_flash \
      0x0 esp32c3/bootloader.bin \
      0x8000 esp32c3/partitions.bin \
      0x10000 esp32c3/firmware.bin

  [ESP32-S3]
    esptool.py --chip esp32s3 --port <포트> --baud 921600 write_flash \
      0x0 esp32s3/bootloader.bin \
      0x8000 esp32s3/partitions.bin \
      0x10000 esp32s3/firmware.bin

  Windows: esptool 대신 esptool.exe, <포트>는 COM3 등
  PlatformIO 사용 시: 각 bin을 해당 주소에 업로드하거나
    pio run -e esp32dev -t upload 로 전체 자동 플래시 가능

빌드 정보:
  - platform: espressif32@6.7.0 (arduino-esp32 2.0.x 고정)
  - NimBLE-Arduino 1.4.2 (lib/ 포함)
  - 파티션: no_ota_spiffs_1mb.csv (app0 2848KB + SPIFFS 1MB)
