================================================================
bitchat ESP32 relay v28 - Flashing + C3 Stability Definitive Fix Guide
================================================================

[v28 Additional Fixes (C3/S3 Reset Loop Resolution)]
  v27 fixed the flashing issue, but C3 Super Mini kept resetting right
  after BLE advertising started. Boot logs revealed 3 problems:
    1. esp_pm_configure failed: 0x0106 (ESP_ERR_NOT_SUPPORTED)
    2. E NimBLEDevice: esp_ble_tx_power_set: rc=-1
    3. LoRa scan calling invalid GPIOs (27/23/26 don't exist on C3)

  v28 fixes:
    A. ble_relay.cpp: Moved NimBLEDevice::setPower() AFTER init()
       - Before: setPower() → init() (calling before controller init → rc=-1)
       - v28:    init() → setPower() (correct order)
       
    B. power.cpp + sdkconfig.defaults.esp32c3: Disabled C3 power management
       - CONFIG_PM_ENABLE=n (skip esp_pm_configure entirely)
       - light_sleep_enable=false (incompatible with active BLE)
       - C3 Super Mini is USB-powered, no need for power saving
    
    C. platformio.ini: Both C3 and S3 now use BC_POWER_USB_ALWAYS=1
       - Switched from battery mode to USB always-on mode
       - Deep sleep / ADC / PM setup all skipped → major stability improvement
    
    D. lora_relay.cpp + main.cpp: Skip LoRa scan on C3
       - C3 doesn't have GPIO 27/23/26 (standard LoRa pinouts invalid)
       - Auto-skip when BOARD_VARIANT_ESP32C3 is defined
       - Use -DBC_LORA_FORCE_SCAN=1 to force scan if external LoRa connected
    
    E. power.h: Disabled battery ADC on C3/S3 Super Mini (GPIO 35 doesn't exist on C3)
       - BC_BATTERY_ADC_PIN=0xFF (C3/S3), =35 (ESP32 classic)
    
    F. power.h: Fixed S3 Super Mini LED pin to GPIO 48 (was 5)

[v27 Definitive Flash Settings (retained)]
  - Added CONFIG_ESPTOOLPY_FLASHMODE_DIO=y / FLASHFREQ_40M=y / FLASHSIZE_4MB=y
    to sdkconfig.defaults
  - bootloader.bin header baked with 4MB/40MHz/DIO at build time

[Problem summary through v26]
  - ESP32-C3 / ESP32-S3 Super Mini boards failed to boot after flashing
  - Symptoms: "waiting for download" infinite loop / SHA mismatch /
    no serial output
  - Root cause: bootloader.bin header generated as 1MB/26MHz
    (firmware.bin was correctly 4MB/40MHz, but only bootloader was wrong)

[Detailed root cause]
  ESP32 image header (first 4 bytes):
    byte 0: 0xE9 (magic)
    byte 1: segment count
    byte 2: SPI mode (0x02=DIO, 0x03=DOUT, ...)
    byte 3: (high nibble) flash size + (low nibble) frequency
            0x20 = 4MB / 40MHz (correct)
            0x02 = 1MB / 26MHz (v26 broken state)

  The board JSON's f_flash/flash_mode only applies to the firmware (app)
  image. The bootloader follows sdkconfig's CONFIG_ESPTOOLPY_* settings.
  v26's sdkconfig.defaults was missing these, so the bootloader was built
  with default values (1MB/26MHz).

[v27 definitive fix]
  1. Added to sdkconfig.defaults (universal):
       CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
       CONFIG_ESPTOOLPY_FLASHFREQ_40M=y
       CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
       CONFIG_ESPTOOLPY_FLASHSIZE="4MB"

  2. Per-target overrides in sdkconfig.defaults.esp32c3 / .esp32s3
     (same flash settings explicitly set per target)

  3. New sdkconfig.defaults.esp32s3 with S3-specific settings:
       - Flash settings (4MB/40MHz/DIO)
       - PSRAM Quad 80MHz (FH4R2 2MB PSRAM)
       - USB CDC (CONFIG_TINYUSB_CDC_ENABLED=y)
       - RSA 2048 + X509 cert write (for cert gen feature)

  4. platformio.ini esp32s3 env changes:
       - Removed `board_build.sdkconfig = sdkconfig.defaults.cdc`
         (was preventing sdkconfig.defaults.esp32s3 from loading)
       - Added explicit `board_build.f_flash = 40000000L`

  5. Deleted sdkconfig.defaults.cdc (settings moved to per-target files)

[Verification]
  $ python3 tools/verify_headers.py
  All boards' bootloader.bin / firmware.bin / merged-flash.bin headers
  are now correctly DIO / 4MB / 40MHz (byte2=0x02, byte3=0x20).

================================================================
Flashing Methods (choose one)
================================================================

[Method A - Unified script (recommended)]

  Step 1: List available serial ports
    python3 tools/flash_bitchat.py --list

    Super Mini should appear as /dev/ttyACM0 or COM3, etc.
    If not visible:
      - Verify USB-C cable supports data (charging-only cables don't work)
      - Hold BOOT button while plugging USB → ROM download mode

  Step 2: Flash
    # ESP32-C3 Super Mini
    python3 tools/flash_bitchat.py --board esp32c3 --port /dev/ttyACM0

    # ESP32-S3 Super Mini (FH4R2)
    python3 tools/flash_bitchat.py --board esp32s3 --port /dev/ttyACM0

    # ESP32 classic (DevKitC, etc.)
    python3 tools/flash_bitchat.py --board esp32dev --port /dev/ttyUSB0

  Step 3 (optional): Verify after flashing
    python3 tools/flash_bitchat.py --board esp32c3 --port /dev/ttyACM0 --verify

  Step 4 (if problems): Disable stub / lower speed / erase all
    python3 tools/flash_bitchat.py --board esp32c3 --port /dev/ttyACM0 \
      --no-stub --baud 460800 --erase-all

[Method B - esptool directly]

  ESP32 classic (bootloader offset is 0x1000 - note!):
    esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
      write_flash --flash-mode dio --flash-freq 40m --flash-size 4MB \
      0x1000  firmware/esp32dev/bootloader.bin \
      0x8000  firmware/esp32dev/partitions.bin \
      0x10000 firmware/esp32dev/firmware.bin

  ESP32-C3 Super Mini (bootloader at 0x0):
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

  Option - flash merged-flash.bin as single blob:
    esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
      write_flash --flash-mode dio --flash-freq 40m --flash-size 4MB \
      0x0 firmware/esp32s3/merged-flash.bin

[Method C - NodeMCU PyFlasher (Windows GUI)]

  NodeMCU PyFlasher can flash merged-flash.bin at offset 0x0.
  - Firmware binary: firmware/esp32c3/merged-flash.bin (or esp32s3/...)
  - Flash mode: dio
  - Flash size: 4MB
  - Flash speed: 40MHz
  - Extra args: --no-stub  (if stub fails on Super Mini)

================================================================
Post-flash verification
================================================================

  1. Press EN button once, or unplug/replug USB to reset
  2. Open serial monitor (115200 baud):
       pio device monitor -p /dev/ttyACM0 -b 115200
       or: screen /dev/ttyACM0 115200
  3. Verify boot messages:
       "bitchat ESP32 relay v27 starting..."
       "BLE relay: advertising started"
       "Storage: ..." etc.

  Super Mini (C3/S3) note:
    - Serial output appears after ~3 seconds (USB CDC enumeration wait)
    - The while(!Serial) wait code is in main.cpp (added in v26)

================================================================
Troubleshooting
================================================================

[Symptom] Serial port not visible at all
  → Verify USB-C cable supports data (charging-only cables don't work)
  → Hold BOOT button while plugging USB → ROM download mode
  → Try different USB port / cable

[Symptom] esptool "Failed to connect to device" error
  → Add --no-stub option (some Super Mini boards fail stub detection)
  → Lower speed: --baud 460800
  → Hold BOOT button while powering on → ROM mode

[Symptom] Flashing succeeds but no boot
  → Verify headers: python3 tools/verify_headers.py
     byte3=0x20 is correct, 0x02 means broken build
  → Use --erase-all to wipe flash before re-flashing (clears NVS residue)

[Symptom] SHA mismatch / boot loop
  → v26's manual header patching of merged-flash.bin broke SHA
  → v27 bakes header at build time → SHA is also correct
  → Individual 3-file flashing is most reliable

[Symptom] ESP32-C3 overheating and reset loop
  → Confirm 40MHz is applied (firmware header byte3=0x20)
  → Disable battery mode: --erase-all and rebuild with esp32c3-usb env
  → Or build with BC_POWER_USB_ALWAYS=1

[Symptom] ESP32-S3 PSRAM not recognized
  → v27 sdkconfig.defaults.esp32s3 has CONFIG_SPIRAM=y
  → Check boot log for "PSRAM initialized" message
  → BOARD_HAS_PSRAM warning is harmless if defined twice

================================================================
Build info
================================================================
  - platform: espressif32@6.7.0 (arduino-esp32 2.0.x)
  - NimBLE-Arduino 1.4.2 (in lib/)
  - Partition: no_ota_spiffs_1280.csv (app0 2688KB + SPIFFS 1280KB)
  - Build environments:
      pio run -e esp32dev   # ESP32 classic
      pio run -e esp32c3    # ESP32-C3 Super Mini
      pio run -e esp32s3    # ESP32-S3 Super Mini (FH4R2)

  Auto-verify after build:
      python3 tools/verify_headers.py
