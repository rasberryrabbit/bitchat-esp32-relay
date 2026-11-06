# bitchat ESP32 Relay Firmware

> ESP32 / ESP32-C3 / ESP32-S3 based bitchat BLE mesh relay node firmware

[한국어 README](README.md) | **English**

This firmware turns an ESP32 into a relay node for the bitchat BLE mesh. It receives packets from nearby bitchat BLE nodes (phones, other ESP32s), verifies signatures, and forwards them to adjacent nodes — extending mesh coverage without internet or cellular.

Additional bridges: **LoRa auto-detection** + **WiFi TCP/UDP/MQTT** for BLE↔LoRa↔Internet bidirectional relay.

---

## v29 — Heat Reduction + Serial Disconnect Reset Fix

v28 disabled power management entirely, causing overheating on C3 Super Mini. Also, closing the serial monitor triggered a reset via DTR/RTS auto-reboot.

### v29 Fixes:

1. **Serial disconnect reset prevention**:
   - S3: `Serial.enableReboot(false)` — disables USBCDC DTR/RTS 4-step reset sequence
   - C3: `monitor_rts=0, monitor_dtr=0` in platformio.ini — prevents PlatformIO monitor from toggling DTR/RTS
   - Alternative: use `screen /dev/ttyACM0 115200` (no DTR/RTS toggling)

2. **DFS (Dynamic Frequency Scaling) re-enabled**:
   - `CONFIG_PM_ENABLE=y` + `light_sleep_enable=false`
   - CPU automatically drops to lower frequency when idle
   - C3: 80→40MHz, S3: 160→80MHz automatic downscale
   - `power_init()` now applies DFS in both USB and battery modes

3. **CPU frequency reduced**:
   - C3: f_cpu 160MHz → **80MHz** (50% reduction, major heat reduction)
   - S3: DFS max_freq 240→160MHz

4. **BLE timing slowed (heat reduction)**:
   - Advertising interval: 3.75-11.25ms → **100-200ms** (1/20 of previous)
   - Scan interval: 80ms/40ms → **1024ms/30ms** (duty 50%→2.9%)
   - Mesh relay discovery still within 1 second — practical performance retained

---

## v28 — C3 Stability Fix (Reset Loop Resolution)

v27 fixed the flashing problem, but C3 Super Mini kept resetting right after BLE advertising started. Three root causes identified and fixed:

1. **TX power call order**: `NimBLEDevice::setPower()` was called BEFORE `NimBLEDevice::init()` → `esp_ble_tx_power_set: rc=-1`. Fixed by reordering.
2. **Power management incompatible with BLE**: `esp_pm_configure` returned `ESP_ERR_NOT_SUPPORTED (0x0106)` on C3 with light sleep enabled. Disabled `CONFIG_PM_ENABLE` for C3.
3. **LoRa scan on invalid GPIOs**: C3 doesn't have GPIO 27/23/26 used by standard LoRa pinouts. Auto-skip LoRa scan on C3 (`-DBC_LORA_FORCE_SCAN=1` to override).

Additional: C3/S3 envs now use `BC_POWER_USB_ALWAYS=1` (USB always-on mode) since Super Mini boards are USB-powered. Battery ADC disabled on C3/S3 (GPIO 35 doesn't exist on C3).

---

## v27 — Definitive Flashing Fix for C3/S3 Super Mini

**Root cause of v26 flashing failures**: `bootloader.bin` was built with 1MB/26MHz flash settings even though `firmware.bin` was correctly 4MB/40MHz. The board JSON's `f_flash`/`flash_mode` only affect the app image; the bootloader follows `sdkconfig`'s `CONFIG_ESPTOOLPY_*` settings, which were missing.

**v27 fix**: Added `CONFIG_ESPTOOLPY_FLASHMODE_DIO=y` / `FLASHFREQ_40M=y` / `FLASHSIZE_4MB=y` to `sdkconfig.defaults` and per-target files. Now `bootloader.bin` is baked with 4MB/40MHz/DIO at build time → no runtime header patching, no SHA mismatch.

Verify your build:
```bash
python3 tools/verify_headers.py
# All boards should show: DIO / 4MB / 40MHz (byte2=0x02, byte3=0x20)
```

Flash your board:
```bash
python3 tools/flash_bitchat.py --board esp32c3 --port /dev/ttyACM0
python3 tools/flash_bitchat.py --board esp32s3 --port /dev/ttyACM0
```

See `README-flash-english.txt` for full flashing guide and troubleshooting.

---

## Supported Boards

| | ESP32 Classic | ESP32-C3 | ESP32-S3 |
|---|---|---|---|
| Chip | ESP-WROOM-32 | ESP32-C3 | ESP32-S3 |
| Core | Xtensa LX6 dual 240MHz | RISC-V RV32 single 160MHz | Xtensa LX7 dual 240MHz |
| SRAM | 520 KB | 400 KB | 512 KB + PSRAM |
| BLE | 4.2 | 5.0 | 5.0 |
| WiFi | 802.11 b/g/n | 802.11 b/g/n | 802.11 b/g/n |

All three boards build from the same source. `BOARD_VARIANT_*` macros auto-detect at compile time.

---

## Key Features

### BLE Mesh Relay
- NimBLE backend (ESP32 BLE 4.2/5.0)
- Max 8 simultaneous BLE connections (4 incoming + 4 outgoing)
- Advertising at 20–50ms interval (discovery priority, battery mode)
- Packet dedup (senderID + timestamp LRU cache, 64 entries)
- TTL-based multi-hop forwarding (max 7 hops)
- Ed25519 signature verification with peer key mapping

### TX Power Control (v17+)
- `txpower <dBm>` Serial command — adjustable BLE TX power (-12 to +9 dBm)
- NVS-persisted, survives reboot
- Default: +9 dBm (max discovery range)

### Packet Dump Toggle (v17+)
- `dump on/off` — runtime hex dump of RX packets
- RAM-only flag (not persisted), default OFF for performance
- 166B packet dump ≈ 73ms Serial blocking → use only for debugging

### Pointer-Based Queue (v18)
- FreeRTOS queue with malloc'd buffers (not inline arrays)
- 64 slots × ~16B = 1KB RAM (was 32 × 752B = 24KB)
- `xQueueSend` with 5ms timeout for burst absorption

### Net Transport — Hybrid TCP/UDP/MQTT (v19–v22)
- **SOCKET TCP**: length-prefix framing, TLS support (`net tls on`)
- **SOCKET UDP**: unicast + ACK retransmission (500/1000/2000ms backoff, max 3 retries)
- **SOCKET UDP-BCAST**: LAN broadcast + ACK (no server needed, P2P mesh)
- **MQTT**: binary payload (no base64), buffer 4096, QoS 0
- **OFF**: WiFi only, no internet bridge
- All settings persisted to SPIFFS `/wifi.conf`

### Open WiFi Auto-Scan (v24)
- No SSID/password configured → automatically scans for open APs
- Selects strongest open AP by RSSI
- Falls back to open WiFi if configured SSID connection fails
- Security warning: use `net tls on` on open networks

### LoRa Bridge (v23)
- SX127x auto-detection (Heltec, TTGO, T-Beam, Dragino pinouts)
- Large packet splitting (222B LoRa frame limit → 208B chunks)
- Inter-chunk delay 200ms (collision avoidance)
- TX retry on failure (1 retry, 100ms backoff)
- Reassembly timeout 15s, 16 concurrent slots

### Store-and-Forward
- RAM cache (32 entries, 20KB) + SPIFFS overflow (3072 records, 811KB)
- Messages stored for offline peers, flushed on reconnect
- `cache spiffs on/off` to control flash writes

### Power Management
- Battery + light sleep mode (default) or USB always-on
- Battery ADC monitoring with V_CRIT/V_LOW thresholds
- `bat ignore on/off` — force BLE keep-alive at critical battery
- `sleep deep on/off` — deep sleep after 5min idle (default OFF)
- Deep sleep armed after 5min idle, 60s wake timer

---

## Build

### PlatformIO (recommended)

```bash
git clone <repo>
cd bitchat-esp32-relay
pio run -e esp32dev     # ESP32 classic
pio run -e esp32c3      # ESP32-C3
pio run -e esp32s3      # ESP32-S3
```

Flash:
```bash
pio run -t upload -e esp32dev
pio device monitor -e esp32dev    # 115200 baud
```

Platform: `espressif32@6.7.0` (pinned).

### Partition Table

`partitions/no_ota_spiffs_1mb.csv`:
- App: 2848KB (no OTA, expanded from default 1280KB)
- SPIFFS: 1024KB (blacklist, peer_keys, msg_store, configs)
- NVS: 24KB (keypair, peers, counter, flags)
- Coredump: 64KB

---

## Serial Commands

All network commands unified under `net`. `wifi` is a deprecated alias.

### Network (`net`)

```
net                      - status (mode + proto + connection)
net on [ssid pass]       - connect WiFi + transport (no SSID = auto open WiFi scan)
net off                  - disconnect everything
net stat                 - detailed stats
net set <ssid> <pass>    - save WiFi credentials (SPIFFS)

net mode socket          - direct TCP/UDP socket (default)
net mode mqtt            - MQTT broker mode
net mode off             - disable internet bridge

net proto tcp            - TCP length-prefix (default, reliable)
net proto udp            - UDP unicast + ACK retransmit (host required)
net proto udp-bcast      - UDP broadcast + ACK (LAN, no host needed)

net host <ip> <port>     - socket server address (SPIFFS persisted)
net host                 - show current socket host

net broker <host> [port] - MQTT broker + port (SPIFFS, MQTT mode)
net prefix <prefix>      - MQTT topic prefix (SPIFFS)
net tls [on|off]         - mTLS toggle (TCP socket + MQTT, port 8883/1883)
```

### BLE / Power

```
txpower                  - show BLE TX power
txpower <N>              - set TX power dBm (-12..9, NVS persisted)

dump                     - show dump status (default OFF)
dump on                  - enable RX hex dump (RAM only, not persisted)
dump off                 - disable dump (performance-safe)

bat                      - battery voltage + ignore status
bat ignore on/off        - force BLE keep-alive at V_CRIT (NVS)
bat read                 - immediate ADC read

sleep                    - power management status
sleep deep on/off        - deep sleep after 5min idle (NVS, default OFF)

cache                    - cache status (RAM + SPIFFS)
cache spiffs on/off      - SPIFFS overflow toggle (NVS)
cache clear              - wipe SPIFFS overflow store
``### Other

```
bl                       - blacklist status
bl add <hex>             - add (32B pk=64hex / 6B ble=12hex)
bl rm <hex>              - remove
bl clear                 - clear all
bl stats                 - counts only

peer                     - peer_keys mappings
peer list                - list all peer keys
peer clear               - clear all

node                     - node profile
node nick <name>         - set nickname (SPIFFS)
node nick clear          - remove nickname

cert status              - show cert files
cert upload <ca|client|key> - paste PEM (end with 'EOF')
cert clear               - delete all certs

help                     - this help
```

---

## SPIFFS Config Files

### `/wifi.conf`
```ini
# bitchat relay wifi/mqtt config
ssid=MyWiFi
pass=secret
broker=test.mosquitto.org
port=1883
topic_prefix=bchat
use_tls=0
net_mode=1
socket_host=192.168.1.100
socket_port=8765
net_proto=0
```

- `net_mode`: 0=off, 1=socket(default), 2=mqtt
- `net_proto`: 0=tcp(default), 1=udp, 2=udp-bcast
- `use_tls`: 0=plain, 1=mTLS (port 8883, certs required)

### `/node.conf`
```ini
nickname=my-relay
```

### Certificate files (for mTLS)
- `/broker_ca.pem` — CA certificate (required for TLS)
- `/client_cert.pem` — client certificate (mTLS)
- `/client_key.pem` — client private key (mTLS)

Upload via `cert upload <ca|client|key>`.

---

## Socket Server (Python)

A Python reference server for the TCP socket mode is included:

```
tools/socket_server/
├── socket_server.py    # TCP hub server (length-prefix protocol)
└── README.md           # usage + protocol docs
```

Run:
```bash
cd tools/socket_server
python3 socket_server.py                 # 0.0.0.0:8765
python3 socket_server.py --port 9000     # custom port
```

Protocol: `[2B big-endian length][payload]` — forwards each frame to all connected relays.

---

## Packet Protocol

bitchat packets (max 744 bytes):
```
[ver(1)][type(1)][ttl(1)][timestamp(8)][flags(1)][payload_len(2)]
[senderID(8)][recipientID(8)][payload(0-650)][signature(64)]
```

Message types:
- `0x01` ANNOUNCE — peer discovery + key exchange
- `0x02` MESSAGE — chat message (broadcast or direct)
- `0x03` LEAVE — peer leaving
- `0x10` NOISE_HANDSHAKE — Noise protocol handshake
- `0x11` NOISE_ENCRYPTED — encrypted direct message
- `0x20` FRAGMENT — large packet fragment
- `0x21` REQUEST_SYNC — message sync request
- `0x22` FILE_TRANSFER — file transfer
- `0x29` VOICE_FRAME — voice frame

---

## File Structure

```
src/
├── main.cpp              - entry point, serial command handler
├── ble_relay.cpp/.h      - NimBLE BLE mesh relay (server + client)
├── bitchat_protocol.cpp/.h - packet parse/build, TTL, types
├── storage.cpp/.h        - NVS: keypair, peers, counter, flags
├── node_config.cpp/.h    - SPIFFS: /node.conf, /wifi.conf
├── wifi_bridge.cpp/.h    - WiFi + TCP/UDP/MQTT bridge
├── lora_relay.cpp/.h     - LoRa SX127x auto-detect + bridge
├── packet_splitter.cpp/.h - large packet split/reassemble (LoRa)
├── power.cpp/.h          - battery ADC, light/deep sleep, LED
├── crypto.cpp/.h         - Ed25519 (libsodium)
├── retry_queue.cpp/.h    - ACK retransmission queue
├── blacklist.cpp/.h      - pk/ble blacklist (SPIFFS)
├── peer_keys.cpp/.h      - senderID → Ed25519 pk mapping
├── msg_cache.cpp/.h      - store-and-forward (RAM + SPIFFS)
├── msg_store.cpp/.h      - SPIFFS message store
├── wifi_bridge.cpp/.h    - WiFi + net transport
└── log.h                 - logging macros (LOGE/W/I/D + hexdump)

lib/
├── NimBLE-Arduino/       - BLE stack (v1.4.2)
├── PubSubClient/         - MQTT client (v2.8)
├── LoRa/                 - SX127x driver (v0.8)
└── (libsodium bundled)

tools/
├── socket_server/        - Python TCP hub server
├── gen_ca.sh             - CA certificate generation
├── gen_client_cert.sh    - client certificate generation
├── rotate_certs.sh       - certificate rotation
└── check_cert_expiry.sh  - cert expiry check
```

---

## BLE Connection Limits

ESP32 BLE controller hardware limits:
- Max 9 simultaneous connections (ESP32 classic)
- Firmware uses 8 (4 incoming + 4 outgoing) for safety margin

For 10+ connections: use multiple ESP32 relay nodes in proximity.

---

## Boot Log Example

```
=============================
 bitchat ESP32 relay firmware
 v24 (open WiFi auto-scan fallback)
 build: Oct 31 2026 01:10:00
 target: ESP32 classic (Xtensa LX6, BLE 4.2)
 max-conn: in=4 out=4 queue=64
 power: BATTERY + light/deep sleep
=============================

[1.054] BLE own addr: ec:62:60:94:d7:0a
[1.062] BLE TX power: +9dBm
[1.079] BLE advertising started as bitchat (NimBLE)
[1.090] BitchatRelay ready (NimBLE backend). max out=4 in=4
```

---

## Limitations

- **BLE address**: public MAC (factory-programmed), stable across reboots
- **BLE coex**: BLE + WiFi simultaneous use has coex overhead; BLE priority set to optimum
- **LoRa**: SX127x only (SX1262/1268 not supported); single frequency channel
- **MQTT**: PubSubClient v2.8 — QoS 0 only (binary payload, no base64)
- **UDP**: max 4 concurrent pending ACK slots (ESP32 RAM constraint)
- **Phone app**: Android bitchat app doesn't restart scan on BT toggle (app-side issue, see GitHub issue report)

---

## License

Public domain (Unlicense). See [LICENSE](LICENSE).

## References

- [bitchat iOS/macOS](https://github.com/permissionlesstech/bitchat)
- [bitchat Android](https://github.com/permissionlesstech/bitchat-android)
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
- [bitchat whitepaper](https://bitchat.free)
