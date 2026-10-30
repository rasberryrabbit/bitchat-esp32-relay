bitchat ESP32 relay firmware - v9 fix (앱 재시작 후 메시지 누락 수정)
=======================================================================

빌드 환경: PlatformIO espressif32@6.7.0 (고정) + arduino-esp32 2.0.17
빌드 결과: 3개(env) 모두 SUCCESS, 0 errors

수정 사항 (v9 - 2026-10-30):
  ★ 이번 수정: "앱을 닫았다 다시 열면 메시지 dump가 안 나오는" 문제

  근본 원인:
    이미지 전송이 중단되면 accumulator에 "가짜 헤더"(이미지 꼬리
    시그니처 바이트가 우연히 패킷 헤더 형태를 띤 것)가 대기 상태로 남음.
    v8까지는 앱 재연결(onConnect) 시 이 버퍼를 비우지 않아서,
    재시작 후 도착하는 단문이 가짜 헤더 뒤에 쌓이고
    "선언된 total_len에 못 미침" 판정 때문에 영원히 추출되지 않았음.
    (v8의 5초 stale 판정은 head 타이머가 갱신되는 버그로 무효화될 수 있었음)

  1. 연결 경계 accumulator 리셋 (핵심 수정, 결정적)
     - onConnect/onDisconnect에서 ble_acc_reset() 호출
     - 앱 재시작 → 재연결 순간 버퍼 100% 초기화 → 항상 깨끗한 상태 시작
     - accumulator 상태를 BitchatRXCallbacks에서 BitchatRelay 멤버로
       승격시켜 서버 콜백에서 접근 가능하게 함

  2. write 간격 감시 → resync 모드 (시간 기반)
     - 정상 패킷 분할은 BLE burst(30~90ms)로 도착
     - 1.5초 이상 write가 끊긴 상태에서 대기 중인 불완전 패킷이 있으면
       가짜 헤더로 판정 → resync 모드 전환
     - v8과 달리 버퍼 전체 폐기가 아니라 1바이트씩 건너뛰어
       대기 head 뒤에 이미 쌓인 정상 패킷을 "구출"함

  3. total_len 초과 판정 (결정적, 수학적으로 참)
     - head가 선언한 total_len을 버퍼가 초과했는데도 파싱 실패면
       그 head는 100% 가짜 (진짜 패킷은 선언 길이에서 반드시 완성)
     - 이 판정으로 스킵 → 뒤따르는 정상 메시지 즉시 구출

  이전 수정 유지 (v3~v8):
    - 폰 연결 중 스캔 금지 (BLE 컨트롤러 점유 방지)
    - 주변 노드 연결 타임아웃 30초 → 3초
    - payload_len 정밀 검증, 22B 미만 잔여물 폐기
    - FNV-1a + timestamp XOR dedup

=== 파일 목록 ===

[ESP32 클래식 (esp32dev) - BLE 4.2]
  bitchat-esp32dev-bootloader.bin   → 0x1000  (17KB)
  bitchat-esp32dev-partitions.bin   → 0x8000  (3KB)
  bitchat-esp32dev.bin              → 0x10000 (1.38MB)

[ESP32-C3 - BLE 5.0, RISC-V]
  bitchat-esp32c3-bootloader.bin    → 0x0     (13KB)
  bitchat-esp32c3-partitions.bin    → 0x8000  (3KB)
  bitchat-esp32c3.bin               → 0x10000 (1.36MB)

[ESP32-S3 - BLE 5.0, 듀얼코어]
  bitchat-esp32s3-bootloader.bin    → 0x0     (15KB)
  bitchat-esp32s3-partitions.bin    → 0x8000  (3KB)
  bitchat-esp32s3.bin               → 0x10000 (1.27MB)

[소스 코드]
  bitchat-esp32-relay-src-fixed.zip (v9 전체 소스, platformio.ini 포함)

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

=== v9 테스트 시나리오 (재현 확인용) ===

1. 앱에서 이미지 전송 중/직후 앱을 닫고 10초 뒤 재실행
   → 재연결 시 "BLE acc: reset on connect" 로그 확인
   → 재실행 직후 보내는 단문이 즉시 dump에 나타나야 함

2. 이미지 전송 직후(1.5초 이상 간격) 단문 전송
   → 필요시 "write gap ... -> resync mode" 로그 후 단문 dump 정상 표시

3. 정상 상황 회귀 테스트
   → 단문/이미지 정상 수신 (burst 내 분할 조립은 무영향)
