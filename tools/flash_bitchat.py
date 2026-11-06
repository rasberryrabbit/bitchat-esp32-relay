#!/usr/bin/env python3
"""
bitchat ESP32 relay v27 - 통합 플래싱 스크립트
지원 보드:
  - esp32dev   : ESP32 클래식 (ESP-WROOM-32, DevKitC, 4MB Flash)
  - esp32c3    : ESP32-C3 Super Mini (4MB Flash, USB CDC)
  - esp32s3    : ESP32-S3 Super Mini (FH4R2: 4MB Flash + 2MB PSRAM, USB CDC)

사용법:
  python3 flash_bitchat.py --board esp32c3 --port /dev/ttyUSB0
  python3 flash_bitchat.py --board esp32s3 --port COM3
  python3 flash_bitchat.py --board esp32dev --port /dev/ttyUSB0 --merged
  python3 flash_bitchat.py --list        # 사용 가능한 시리얼 포트 목록

옵션:
  --board BOARD    : esp32dev / esp32c3 / esp32s3 (필수)
  --port PORT      : 시리얼 포트 경로 (필수, --list로 확인)
  --merged         : merged-flash.bin 하나로 플래싱 (개별 3파일 대신)
                     주의: 일부 보드에서 병합 파일 방식이 느림.
                     기본은 개별 3파일 플래싱.
  --baud BAUD      : 플래싱 속도 (기본 921600, 느린 보드는 460800)
  --no-stub        : esptool stub 비활성 (ROM bootloader만 사용)
                     일부 Super Mini 보드에서 stub 인식 실패 시 사용.
  --erase-all      : 플래시 전체 삭제 후 플래싱 (NVS 초기화 필요 시)
  --list           : 사용 가능한 시리얼 포트 출력 후 종료
  --verify         : 플래싱 후 바이너리 검증
"""
import argparse
import os
import subprocess
import sys
import glob

# 보드별 설정
BOARD_CONFIG = {
    'esp32dev': {
        'chip': 'esp32',
        'bootloader_offset': '0x1000',
        'partitions_offset': '0x8000',
        'firmware_offset': '0x10000',
        'flash_mode': 'dio',
        'flash_freq': '40m',
        'flash_size': '4MB',
        'description': 'ESP32 클래식 (ESP-WROOM-32, DevKitC)',
    },
    'esp32c3': {
        'chip': 'esp32c3',
        'bootloader_offset': '0x0',
        'partitions_offset': '0x8000',
        'firmware_offset': '0x10000',
        'flash_mode': 'dio',
        'flash_freq': '40m',
        'flash_size': '4MB',
        'description': 'ESP32-C3 Super Mini (4MB, USB CDC)',
    },
    'esp32s3': {
        'chip': 'esp32s3',
        'bootloader_offset': '0x0',
        'partitions_offset': '0x8000',
        'firmware_offset': '0x10000',
        'flash_mode': 'dio',
        'flash_freq': '40m',
        'flash_size': '4MB',
        'description': 'ESP32-S3 Super Mini (FH4R2: 4MB + 2MB PSRAM, USB CDC)',
    },
}

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
# 스크립트가 tools/ 안에 있으므로 상위 firmware/ 디렉토리 사용
FIRMWARE_DIR = os.path.join(SCRIPT_DIR, '..', 'firmware')
if not os.path.isdir(FIRMWARE_DIR):
    FIRMWARE_DIR = os.path.join(SCRIPT_DIR, 'firmware')  # fallback


def find_esptool():
    """esptool.py 실행 파일 위치 탐색"""
    # 1. PATH에서 직접 탐색
    for cmd in ['esptool.py', 'esptool']:
        try:
            subprocess.run([cmd, '--version'], capture_output=True, check=True, timeout=5)
            return cmd
        except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired):
            continue
    # 2. PlatformIO 캐시 탐색
    home = os.path.expanduser('~')
    candidates = [
        os.path.join(home, '.platformio', 'penv', 'bin', 'esptool.py'),
        os.path.join(home, '.platformio', 'packages', 'tool-esptoolpy', 'esptool.py'),
        os.path.join(home, '.venv', 'bin', 'esptool.py'),
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return None


def list_ports():
    """사용 가능한 시리얼 포트 출력"""
    print("사용 가능한 시리얼 포트:")
    found = False
    patterns = [
        '/dev/ttyUSB*',
        '/dev/ttyACM*',
        '/dev/tty.usbserial*',
        '/dev/tty.usbmodem*',
        '/dev/cu.usbserial*',
        '/dev/cu.usbmodem*',
    ]
    for pat in patterns:
        for p in sorted(glob.glob(pat)):
            print(f"  {p}")
            found = True
    # Windows COM ports
    if sys.platform == 'win32':
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r'HARDWARE\\DEVICEMAP\\SERIALCOMM') as key:
                i = 0
                while True:
                    try:
                        name, value, _ = winreg.EnumValue(key, i)
                        print(f"  {value}")
                        found = True
                        i += 1
                    except OSError:
                        break
        except Exception:
            pass
    if not found:
        print("  (시리얼 포트를 찾을 수 없습니다. USB 케이블 연결을 확인하세요.)")
        print("  팁: Super Mini 보드는 데이터 통신 지원 USB-C 케이블이 필요합니다.")
        print("       충전 전용 케이블은 데이터 전송이 안 됩니다.")
    return found


def verify_firmware_files(board):
    """펌웨어 파일 존재 확인"""
    fw_dir = os.path.join(FIRMWARE_DIR, board)
    if not os.path.isdir(fw_dir):
        print(f"오류: 펌웨어 디렉토리가 없습니다: {fw_dir}")
        print(f"     firmware/{board}/ 아래에 bootloader.bin, partitions.bin, firmware.bin이 있어야 합니다.")
        return False
    required = ['bootloader.bin', 'partitions.bin', 'firmware.bin']
    for f in required:
        p = os.path.join(fw_dir, f)
        if not os.path.exists(p):
            print(f"오류: 필수 파일이 없습니다: {p}")
            return False
    return True


def flash_individual(esptool, cfg, port, baud, no_stub, verify, erase_all):
    """개별 파일 3개 플래싱 (권장)"""
    fw_dir = os.path.join(FIRMWARE_DIR, args.board)
    files = [
        (cfg['bootloader_offset'], os.path.join(fw_dir, 'bootloader.bin')),
        (cfg['partitions_offset'], os.path.join(fw_dir, 'partitions.bin')),
        (cfg['firmware_offset'],  os.path.join(fw_dir, 'firmware.bin')),
    ]

    cmd = [esptool, '--chip', cfg['chip']]
    if no_stub:
        cmd.append('--no-stub')
    cmd.extend(['--port', port, '--baud', str(baud)])
    if erase_all:
        cmd.append('erase_flash')
        print("=== 1단계: 전체 플래시 삭제 ===")
        print(' '.join(cmd))
        result = subprocess.run(cmd)
        if result.returncode != 0:
            print("오류: 플래시 삭제 실패")
            return False
        cmd = [esptool, '--chip', cfg['chip']]
        if no_stub:
            cmd.append('--no-stub')
        cmd.extend(['--port', port, '--baud', str(baud)])

    cmd.extend([
        'write_flash',
        '--flash-mode', cfg['flash_mode'],
        '--flash-freq', cfg['flash_freq'],
        '--flash-size', cfg['flash_size'],
    ])
    if verify:
        cmd.append('--verify')
    for offset, path in files:
        cmd.extend([offset, path])

    print("=== 플래싱 시작 (개별 파일 3개) ===")
    print(f"  보드: {args.board} ({cfg['description']})")
    print(f"  포트: {port}")
    print(f"  속도: {baud} baud")
    print(f"  플래시: {cfg['flash_mode']} / {cfg['flash_freq']} / {cfg['flash_size']}")
    print(f"  stub: {'비활성' if no_stub else '활성'}")
    print()
    print(' '.join(cmd))
    print()
    result = subprocess.run(cmd)
    return result.returncode == 0


def flash_merged(esptool, cfg, port, baud, no_stub, verify):
    """merged-flash.bin 단일 파일 플래싱"""
    merged_path = os.path.join(FIRMWARE_DIR, args.board, 'merged-flash.bin')
    if not os.path.exists(merged_path):
        print(f"오류: merged-flash.bin이 없습니다: {merged_path}")
        return False

    cmd = [esptool, '--chip', cfg['chip']]
    if no_stub:
        cmd.append('--no-stub')
    cmd.extend(['--port', port, '--baud', str(baud)])
    cmd.extend([
        'write_flash',
        '--flash-mode', cfg['flash_mode'],
        '--flash-freq', cfg['flash_freq'],
        '--flash-size', cfg['flash_size'],
    ])
    if verify:
        cmd.append('--verify')
    cmd.extend(['0x0', merged_path])

    print("=== 플래싱 시작 (merged-flash.bin) ===")
    print(f"  보드: {args.board} ({cfg['description']})")
    print(f"  포트: {port}")
    print(f"  속도: {baud} baud")
    print(f"  플래시: {cfg['flash_mode']} / {cfg['flash_freq']} / {cfg['flash_size']}")
    print(f"  stub: {'비활성' if no_stub else '활성'}")
    print()
    print(' '.join(cmd))
    print()
    result = subprocess.run(cmd)
    return result.returncode == 0


def verify_header(esptool, cfg, port):
    """플래싱된 bootloader 헤더를 읽어 검증 (4MB/40MHz/DIO)"""
    print("=== 헤더 검증 ===")
    cmd = [esptool, '--chip', cfg['chip'], '--port', port,
           'read_flash', cfg['bootloader_offset'], '4', '/tmp/header.bin']
    result = subprocess.run(cmd, capture_output=True)
    if result.returncode != 0:
        print("경고: 헤더 검증 실패 (플래싱은 성공했을 수 있음)")
        return
    with open('/tmp/header.bin', 'rb') as f:
        data = f.read(4)
    print(f"  읽은 헤더: {data.hex()}")
    if len(data) >= 4 and data[0] == 0xe9 and data[2] == 0x02 and data[3] == 0x20:
        print("  ✓ 정상: DIO / 4MB / 40MHz 헤더 확인됨")
    else:
        print("  ✗ 경고: 헤더가 예상과 다릅니다. 플래싱을 다시 시도하세요.")
        if data[3] == 0x02:
            print("    → 1MB/26MHz 헤더 (잘못된 빌드 사용 중)")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='bitchat ESP32 relay v27 플래싱 스크립트',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument('--board', choices=list(BOARD_CONFIG.keys()),
                        help='대상 보드 (esp32dev / esp32c3 / esp32s3)')
    parser.add_argument('--port', help='시리얼 포트 경로')
    parser.add_argument('--merged', action='store_true',
                        help='merged-flash.bin 단일 파일 사용 (기본: 개별 3파일)')
    parser.add_argument('--baud', type=int, default=921600,
                        help='플래싱 속도 (기본 921600)')
    parser.add_argument('--no-stub', action='store_true',
                        help='esptool stub 비활성 (ROM bootloader만)')
    parser.add_argument('--erase-all', action='store_true',
                        help='플래시 전체 삭제 후 플래싱 (NVS 초기화)')
    parser.add_argument('--verify', action='store_true',
                        help='플래싱 후 바이너리 검증')
    parser.add_argument('--list', action='store_true',
                        help='사용 가능한 시리얼 포트 출력')
    args = parser.parse_args()

    if args.list:
        list_ports()
        sys.exit(0)

    if not args.board:
        parser.error('--board는 필수입니다. (esp32dev / esp32c3 / esp32s3)')
    if not args.port:
        parser.error('--port는 필수입니다. --list로 사용 가능한 포트를 확인하세요.')

    esptool = find_esptool()
    if not esptool:
        print("오류: esptool을 찾을 수 없습니다.")
        print("설치: pip install esptool")
        sys.exit(1)
    print(f"esptool 위치: {esptool}")

    if not verify_firmware_files(args.board):
        sys.exit(1)

    cfg = BOARD_CONFIG[args.board]

    if args.merged:
        ok = flash_merged(esptool, cfg, args.port, args.baud, args.no_stub, args.verify)
    else:
        ok = flash_individual(esptool, cfg, args.port, args.baud, args.no_stub, args.verify, args.erase_all)

    if ok:
        print()
        print("=" * 60)
        print("✓ 플래싱 완료!")
        print("=" * 60)
        print()
        print("다음 단계:")
        print("  1. 보드의 EN 버튼을 한 번 누르거나 USB를 뺐다가 다시 꽂아 리셋")
        print("  2. 시리얼 모니터 열기 (115200 baud):")
        print(f"     pio device monitor -p {args.port} -b 115200")
        print(f"     또는: screen {args.port} 115200")
        print()
        print("  Super Mini (C3/S3) 참고:")
        print("    - 시리얼 출력이 처음 3초간 대기 후 나타납니다 (USB CDC 열거 대기)")
        print("    - 시리얼 포트가 보이지 않으면:")
        print("      * 데이터 통신 지원 USB-C 케이블인지 확인")
        print("      * BOOT 버튼을 누른 채 USB 꽂기 → ROM 다운로드 모드 진입")
        sys.exit(0)
    else:
        print()
        print("✗ 플래싱 실패")
        print()
        print("문제 해결:")
        print("  1. 올바른 보드인지 확인: --board esp32c3 또는 esp32s3")
        print("  2. 올바른 포트인지 확인: --list 로 포트 목록 조회")
        print("  3. stub 오류 시: --no-stub 옵션 추가")
        print("  4. 속도 문제 시: --baud 460800 으로 속도 낮춤")
        print("  5. BOOT 버튼을 누른 채 USB 꽂기 → ROM 다운로드 모드 진입")
        sys.exit(1)
