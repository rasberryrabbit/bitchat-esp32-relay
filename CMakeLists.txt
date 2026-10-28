# CMakeLists.txt - ESP-IDF root project
#
# 빌드 방법:
#   1. components/ 하위에 espressif/arduino-esp32를 클론:
#      cd components/
#      git clone https://github.com/espressif/arduino-esp32.git arduino
#      cd arduino && git submodule update --init --recursive
#
#   2. ESP-IDF 환경 설정 후 빌드:
#      idf.py set-target esp32
#      idf.py build
#      idf.py -p /dev/ttyUSB0 flash monitor
#
# 자세한 사항은 README.md 의 "ESP-IDF 빌드" 섹션 참고.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake OPTIONAL)

project(bitchat-esp32-relay)

# arduino-esp32 컴포넌트를 사용하려면 components/arduino 폴더에
# arduino-esp32 저장소를 클론해두어야 함 (위 주석 참고)
