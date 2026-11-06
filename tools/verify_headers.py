#!/usr/bin/env python3
"""
bitchat ESP32 relay v27 - 빌드 산물 헤더 검증 스크립트
모든 보드의 bootloader.bin / firmware.bin / merged-flash.bin 헤더가
DIO / 4MB / 40MHz 설정으로 올바르게 빌드되었는지 확인.

사용법:
  python3 verify_headers.py
  python3 verify_headers.py --dir /path/to/firmware
"""
import os
import sys
import argparse
import struct

# ESP 이미지 헤더 매직
ESP_MAGIC = 0xE9

# Flash mode 값
FLASH_MODES = {0: 'QIO', 1: 'QOUT', 2: 'DIO', 3: 'DOUT'}

# Flash size (high nibble of byte 3)
FLASH_SIZES = {0: '1MB', 1: '2MB', 2: '4MB', 3: '8MB', 4: '16MB', 5: '32MB'}

# Flash freq (low nibble of byte 3)
FLASH_FREQS = {0: '40MHz', 1: '20MHz', 2: '26MHz', 0xF: '80MHz'}


def parse_header(data):
    """ESP 이미지 헤더 파싱 (처음 4바이트)"""
    if len(data) < 4:
        return None
    magic = data[0]
    segments = data[1]
    spi_mode = data[2]
    spi_size_freq = data[3]
    size_id = (spi_size_freq >> 4) & 0xF
    freq_id = spi_size_freq & 0xF
    return {
        'magic': magic,
        'magic_ok': magic == ESP_MAGIC,
        'segments': segments,
        'spi_mode': spi_mode,
        'spi_mode_name': FLASH_MODES.get(spi_mode, f'UNKNOWN(0x{spi_mode:02x})'),
        'size_id': size_id,
        'size_name': FLASH_SIZES.get(size_id, f'UNKNOWN(0x{size_id:x})'),
        'freq_id': freq_id,
        'freq_name': FLASH_FREQS.get(freq_id, f'UNKNOWN(0x{freq_id:x})'),
        'byte3': spi_size_freq,
    }


def check_file(path, expected_offset=0):
    """단일 .bin 파일의 헤더 검증"""
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as f:
        f.seek(expected_offset)
        data = f.read(4)
    return parse_header(data)


def main():
    parser = argparse.ArgumentParser(description='v27 빌드 산물 헤더 검증')
    parser.add_argument('--dir', default=None,
                        help='firmware 디렉토리 경로 (기본: 스크립트 인접 firmware/)')
    args = parser.parse_args()

    if args.dir:
        fw_dir = args.dir
    else:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        # 스크립트가 tools/ 안에 있으므로 상위 firmware/ 디렉토리 탐색
        fw_dir = os.path.join(script_dir, '..', 'firmware')
        if not os.path.isdir(fw_dir):
            fw_dir = os.path.join(script_dir, 'firmware')  # fallback

    if not os.path.isdir(fw_dir):
        print(f"오류: firmware 디렉토리가 없습니다: {fw_dir}")
        sys.exit(1)

    all_ok = True
    boards = ['esp32dev', 'esp32c3', 'esp32s3']
    # expected: DIO(0x02) / 4MB(size=0x2) / 40MHz(freq=0x0) → byte3=0x20
    EXPECTED_BYTE3 = 0x20
    EXPECTED_MODE = 0x02

    print("=" * 78)
    print("bitchat ESP32 relay v27 - 헤더 검증")
    print("예상: DIO mode / 4MB / 40MHz (byte2=0x02, byte3=0x20)")
    print("=" * 78)
    print()

    for board in boards:
        bdir = os.path.join(fw_dir, board)
        if not os.path.isdir(bdir):
            print(f"[{board}] 펌웨어 디렉토리 없음 - 건너뜀")
            continue
        print(f"[{board}]")
        files_to_check = [
            ('bootloader.bin', 0),
            ('firmware.bin', 0),
            ('partitions.bin', 0),  # partition table (different magic)
            ('merged-flash.bin', 0x1000 if board == 'esp32dev' else 0x0),
        ]
        for fname, offset in files_to_check:
            fpath = os.path.join(bdir, fname)
            if not os.path.exists(fpath):
                print(f"  {fname:25s} - 파일 없음")
                continue
            h = check_file(fpath, offset)
            if h is None:
                print(f"  {fname:25s} - 읽기 실패")
                continue
            if fname == 'partitions.bin':
                # partition table: magic = 0xAA 0x50
                ok = (h['magic'] == 0xAA)
                status = '✓' if ok else '✗'
                print(f"  {fname:25s} {status} magic=0x{h['magic']:02x} (partitions magic)")
                if not ok:
                    all_ok = False
            else:
                ok = (h['magic_ok'] and h['spi_mode'] == EXPECTED_MODE
                      and h['byte3'] == EXPECTED_BYTE3)
                status = '✓' if ok else '✗'
                print(f"  {fname:25s} {status} "
                      f"magic=0x{h['magic']:02x} mode={h['spi_mode_name']} "
                      f"size={h['size_name']} freq={h['freq_name']} "
                      f"(byte2=0x{h['spi_mode']:02x} byte3=0x{h['byte3']:02x})")
                if not ok:
                    all_ok = False
                    if h['byte3'] == 0x02:
                        print(f"    !! 1MB/26MHz 헤더 - v26 이전 빌드 산물입니다.")
                        print(f"    !! sdkconfig.defaults에 CONFIG_ESPTOOLPY_* 설정이 누락되어 있습니다.")
        print()

    print("=" * 78)
    if all_ok:
        print("✓ 모든 헤더 정상 - 플래싱 가능 상태")
        print()
        print("플래싱:")
        print("  python3 tools/flash_bitchat.py --board esp32c3 --port /dev/ttyUSB0")
        print("  python3 tools/flash_bitchat.py --board esp32s3 --port /dev/ttyACM0")
        sys.exit(0)
    else:
        print("✗ 헤더 이상 발견 - 빌드 설정 확인 필요")
        print()
        print("점검 항목:")
        print("  1. sdkconfig.defaults에 CONFIG_ESPTOOLPY_FLASHMODE_DIO=y")
        print("     CONFIG_ESPTOOLPY_FLASHFREQ_40M=y")
        print("     CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y 포함 확인")
        print("  2. sdkconfig.defaults.esp32c3 / .esp32s3 파일 존재 및 동일 설정 포함 확인")
        print("  3. .pio 디렉토리 삭제 후 pio run -e esp32c3 다시 빌드")
        sys.exit(1)


if __name__ == '__main__':
    main()
