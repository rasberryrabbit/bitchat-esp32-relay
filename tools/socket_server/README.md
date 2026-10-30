# bitchat socket bridge server

Python example for the v19 direct-socket transport. Run this on a machine
that all your ESP32 relays can reach (LAN, VPN, or public IP), and point each
relay at it via Serial:

```
net mode socket
net host <server-ip> 8765
wifi on
```

## Requirements

- Python 3.6+ (standard library only, no pip installs)

## Run

```
python3 socket_server.py                 # 0.0.0.0:8765
python3 socket_server.py --port 9000     # custom port
python3 socket_server.py --host 127.0.0.1 --port 8765
```

## Protocol

Wire frame (little-endian NOT used — length is **big-endian**):

```
+----------------+-------------------+
| 2 bytes        | length bytes      |
| length (BE)    | payload (binary)  |
+----------------+-------------------+
```

- `length` is the byte count of the payload that follows.
- `payload` is a raw bitchat mesh packet (binary, same bytes the ESP32 would
  forward over BLE). No base64, no JSON.
- Max payload: 744 bytes (`BITCHAT_MAX_PKT_LEN`). Larger frames are dropped
  and the offending connection is closed.
- The server forwards every received frame verbatim to all other connected
  relays. It never inspects or modifies packet contents.

## Security notes

This example has **no authentication and no encryption**. Anyone who can
reach the port can connect and see every packet. For real deployments:

1. **Behind a VPN** — run the server on a private network and let relays
   reach it over WireGuard/Tailscale/etc.
2. **SSH tunnel** — `ssh -L 8765:localhost:8765 user@server`, then point the
   relay at `127.0.0.1 8765` (relay and SSH client on same LAN).
3. **Reverse proxy + TLS** — terminate TLS in front of the Python server and
   run the relay with a TLS-capable client. (The ESP32 socket client in v19
   uses plain TCP; a TLS variant would need `WiFiClientSecure`.)

## Multiple relays

The server is a hub: any frame sent by one relay is copied to every other
connected relay. This lets you build a star topology where several ESP32
relays in different locations exchange packets through a central server,
without any of them needing a public IP or an MQTT broker.

```
   [ESP32 A] ──┐
               ├── [socket_server.py] ── [ESP32 C]
   [ESP32 B] ──┘
```

A → B delivery: A sends frame to server, server forwards to B and C.
C ignores packets not addressed to it; B decrypts if it is the recipient.

## Debug

The server prints a line per forwarded frame:

```
[14:32:01] fwd 166B from ('192.168.1.42', 49152) to 2 peer(s) (total=18)
```

`total` is the running count of forwarded frames since server start.
