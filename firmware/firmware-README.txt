bitchat ESP32 relay firmware - v3 fix (BLE scan/connect 억제)
==============================================================

빌드 환경: PlatformIO espressif32@6.7.0 + arduino-esp32 2.0.17
빌드 결과: 3개 모두 SUCCESS, 0 errors

수정 사항 (v3 - 2026-10-29):
  1. BLE 연결 타임아웃 단축: 30초 → 3초
     - client->setConnectTimeout(3) 추가
     - 주변 노드 연결 실패 시 3초 후 BLE 해제 (이전 31초 점유)

  2. 데이터 수신 중 스캔 억제: 최근 3초 내 패킷 수신 시 scan_cycle 스킵
     - _last_rx_ms 변수 추가
     - 이미지/단문 수신 중에는 스캔/연결 안 함 → 폰 통신 보장

  3. Accumulator fix v2 유지: payload_len 정밀 검증
     - 가비지 byte[0]=0x01 + 유효type 패턴도 payload_len으로 구분

  4. Dedup fix 유지: FNV-1a hash + timestamp 하위 4바이트 XOR

=== 파일 목록 ===

[ESP32 클래식 (esp32dev) - BLE 4.2]
  bitchat-esp32dev-bootloader.bin   → 0x1000  (17KB)
  bitchat-esp32dev-partitions.bin   → 0x8000  (3KB)
  bitchat-esp32dev.bin              → 0x10000 (1.37MB)

[ESP32-C3 - BLE 5.0, RISC-V]
  bitchat-esp32c3-bootloader.bin    → 0x0     (13KB)
  bitchat-esp32c3-partitions.bin    → 0x8000  (3KB)
  bitchat-esp32c3.bin               → 0x10000 (1.35MB)

[ESP32-S3 - BLE 5.0, 듀얼코어]
  bitchat-esp32s3-bootloader.bin    → 0x0     (15KB)
  bitchat-esp32s3-partitions.bin    → 0x8000  (3KB)
  bitchat-esp32s3.bin               → 0x10000 (1.27MB)

=== 플래싱 명령 ===

# ESP32 클래식 (bootloader = 0x1000)
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0x1000  bitchat-esp32dev-bootloader.bin \
              0x8000  bitchat-esp32dev-partitions.bin \
              0x10000 bitchat-esp32dev.bin

# ESP32-C3 (bootloader = 0x0)
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
  write_flash 0x0     bitchat-esp32c3-bootloader.bin \
              0x8000  bitchat-esp32c3-partitions.bin \
              0x10000 bitchat-esp32c3.bin

# ESP32-S3 (bootloader = 0x0)
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0x0     bitchat-esp32s3-bootloader.bin \
              0x8000  bitchat-esp32s3-partitions.bin \
              0x10000 bitchat-esp32s3.bin

# 이후 펌웨어만 업데이트 (모든 보드 공통)
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0x10000 bitchat-esp32dev.bin
