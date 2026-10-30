#!/usr/bin/env python3
"""
bitchat ESP32 relay - socket bridge server example

Listens on TCP port 8765 (default) and accepts connections from ESP32 relay
nodes running in SOCKET net mode. Each frame on the wire is:

    [2 bytes big-endian length][length bytes payload]

The payload is a raw bitchat mesh packet (binary, NOT base64). The server
forwards every received frame to all other connected relay clients, acting
as a simple hub so multiple ESP32 relays (or a relay + a laptop bridge) can
exchange packets over the internet without an MQTT broker.

Usage:
    python3 socket_server.py                 # listen 0.0.0.0:8765
    python3 socket_server.py --port 9000     # custom port
    python3 socket_server.py --host 0.0.0.0 --port 8765

Protocol notes:
    - Frame length is capped at 744 bytes (BITCHAT_MAX_PKT_LEN on ESP32).
      Larger frames are dropped and the connection is closed.
    - The server does not interpret packet contents; it forwards bytes as-is.
    - No authentication/encryption. Run behind a VPN or reverse proxy with TLS
      if you need confidentiality. For a quick secure tunnel, consider:
          ssh -L 8765:localhost:8765 user@your-server
      and point the ESP32 at 127.0.0.1:8765 via `net host 127.0.0.1 8765`
      (requires the ESP32 and the SSH client to be on the same LAN).

ESP32-side setup (Serial commands):
    net mode socket          # default, but explicit
    net host <server-ip> 8765
    wifi on                  # if WiFi creds already saved via `wifi set <s> <p>`
"""

import argparse
import socket
import struct
import threading
import select
import time
import sys

MAX_FRAME_LEN = 744   # BITCHAT_MAX_PKT_LEN on ESP32
HEADER_LEN = 2        # big-endian uint16 length


class RelayClient:
    """One connected ESP32 relay (or bridge)."""

    def __init__(self, conn, addr, hub):
        self.conn = conn
        self.addr = addr
        self.hub = hub
        self.alive = True
        self.rx_buf = b""
        self.lock = threading.Lock()

    def fileno(self):
        return self.conn.fileno()

    def close(self):
        self.alive = False
        try:
            self.conn.close()
        except Exception:
            pass

    def send_frame(self, payload: bytes):
        """Send a framed payload to this client."""
        if not self.alive:
            return
        frame = struct.pack(">H", len(payload)) + payload
        try:
            self.conn.sendall(frame)
        except (BrokenPipeError, ConnectionResetError, OSError):
            self.close()

    def on_data(self, data: bytes):
        """Feed received bytes; emit complete frames to the hub."""
        self.rx_buf += data
        while len(self.rx_buf) >= HEADER_LEN:
            (length,) = struct.unpack(">H", self.rx_buf[:HEADER_LEN])
            if length == 0 or length > MAX_FRAME_LEN:
                print(f"[{self.addr}] invalid frame length {length}, closing")
                self.close()
                return
            if len(self.rx_buf) < HEADER_LEN + length:
                break  # wait for the rest of the payload
            payload = self.rx_buf[HEADER_LEN:HEADER_LEN + length]
            self.rx_buf = self.rx_buf[HEADER_LEN + length:]
            self.hub.broadcast(payload, source=self)


class Hub:
    """Holds all connected clients and broadcasts frames between them."""

    def __init__(self):
        self.clients = []
        self.lock = threading.Lock()
        self.packets_total = 0

    def add(self, client):
        with self.lock:
            self.clients.append(client)
        print(f"[+] {client.addr} connected ({len(self.clients)} total)")

    def remove(self, client):
        with self.lock:
            if client in self.clients:
                self.clients.remove(client)
        print(f"[-] {client.addr} disconnected ({len(self.clients)} total)")

    def broadcast(self, payload: bytes, source: RelayClient):
        """Forward payload to every client except the source."""
        self.packets_total += 1
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] fwd {len(payload)}B from {source.addr} "
              f"to {len(self.clients) - 1} peer(s) (total={self.packets_total})")
        with self.lock:
            targets = [c for c in self.clients if c is not source and c.alive]
        for c in targets:
            c.send_frame(payload)


def serve(host: str, port: int):
    hub = Hub()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(16)
    print(f"bitchat socket bridge listening on {host}:{port}")
    print(f"  frame format: [2B BE length][payload], max payload {MAX_FRAME_LEN}B")
    print(f"  Ctrl-C to stop")

    try:
        while True:
            rlist = [srv] + [c for c in hub.clients if c.alive]
            readable, _, _ = select.select(rlist, [], [], 1.0)
            for s in readable:
                if s is srv:
                    conn, addr = srv.accept()
                    conn.setblocking(False)
                    client = RelayClient(conn, addr, hub)
                    hub.add(client)
                else:
                    try:
                        data = s.conn.recv(4096)
                    except (ConnectionResetError, OSError):
                        data = b""
                    if not data:
                        hub.remove(s)
                        s.close()
                    else:
                        s.on_data(data)
            # prune dead clients
            for c in list(hub.clients):
                if not c.alive:
                    hub.remove(c)
                    c.close()
    except KeyboardInterrupt:
        print("\nshutting down...")
    finally:
        for c in hub.clients:
            c.close()
        srv.close()


def main():
    ap = argparse.ArgumentParser(description="bitchat ESP32 socket bridge server")
    ap.add_argument("--host", default="0.0.0.0", help="bind host (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=8765, help="listen port (default 8765)")
    args = ap.parse_args()
    serve(args.host, args.port)


if __name__ == "__main__":
    main()
