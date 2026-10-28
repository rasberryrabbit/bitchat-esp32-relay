#!/usr/bin/env bash
# setup_arduino_ide.sh - src/의 소스파일을 Arduino IDE 스케치 폴더로 복사
#
# 사용법:
#   cd /path/to/bitchat-esp32-relay
#   bash setup_arduino_ide.sh
#
# 생성 결과:
#   arduino_ide/bitchat_relay/bitchat_relay.ino
#   arduino_ide/bitchat_relay/*.cpp *.h
#
# Arduino IDE에서 빌드하려면:
#   1. arduino_ide/bitchat_relay/bitchat_relay.ino 파일을 더블클릭
#   2. 보드: ESP32 Dev Module
#   3. Upload

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
ARDUINO_DIR="$PROJECT_ROOT/arduino_ide/bitchat_relay"

echo "[1/4] 폴더 생성: $ARDUINO_DIR"
mkdir -p "$ARDUINO_DIR"

echo "[2/4] src/*.cpp *.h 복사 -> arduino_ide/bitchat_relay/"
cp "$PROJECT_ROOT/src/"*.cpp "$ARDUINO_DIR/"
cp "$PROJECT_ROOT/src/"*.h   "$ARDUINO_DIR/"

# main.cpp는 이미 setup()/loop()를 포함하고 있으므로 .ino로 복사
# (Arduino IDE는 .ino가 있으면 그것을 메인으로 인식)
echo "[3/4] main.cpp -> bitchat_relay.ino 복사"
cp "$PROJECT_ROOT/src/main.cpp" "$ARDUINO_DIR/bitchat_relay.ino"

# Arduino IDE가 .ino와 .cpp를 같이 컴파일하면 main.cpp의 setup()/loop()가
# 두 번 정의되므로 .cpp 파일은 삭제
echo "[4/4] main.cpp 제거 (중복 정의 방지)"
rm -f "$ARDUINO_DIR/main.cpp"

echo ""
echo "완료! Arduino IDE에서 아래 파일을 여세요:"
echo "  $ARDUINO_DIR/bitchat_relay.ino"
echo ""
echo "보드 선택 (사용 중인 보드에 따라):"
echo "  - ESP32 클래식: Tools > Board > ESP32 Arduino > ESP32 Dev Module"
echo "  - ESP32-C3:      Tools > Board > ESP32 Arduino > ESP32C3 Dev Module"
echo "                  (또는 Seeed XIAO ESP32C3)"
echo ""
echo "업로드 속도: Tools > Upload Speed > 921600 (C3 USB CDC는 115200 추천)"
echo "포트: Tools > Port > /dev/ttyUSB0 또는 /dev/ttyACM0 (C3)"
echo ""
echo "참고: 보드를 선택하면 CONFIG_IDF_TARGET_ESP32C3 매크로가 자동 정의되어"
echo "      소스가 C3용 설정(연결 수, 큐 크기)로 자동 조정됩니다."
