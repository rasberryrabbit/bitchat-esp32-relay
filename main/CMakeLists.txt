# main/CMakeLists.txt - ESP-IDF main component
#
# src/ 폴더의 모든 .cpp 파일을 컴파일하여 하나의 main 컴포넌트로 만듦.
# arduino-esp32 컴포넌트가 이미 추가되어 있어야 함 (project root의
# CMakeLists.txt 주석 참고).

idf_component_register(
    SRCS
        "../src/main.cpp"
        "../src/bitchat_protocol.cpp"
        "../src/crypto.cpp"
        "../src/storage.cpp"
        "../src/ble_relay.cpp"
    INCLUDE_DIRS
        "../src"
    REQUIRES
        arduino
        nvs_flash
        mbedtls
        bt
)

# C++17 표준 활성화 (Arduino API 호환)
target_compile_options(${COMPONENT_LIB} PUBLIC -std=gnu++17)
