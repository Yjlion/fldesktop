#!/usr/bin/env python3
# A VNC server that speaks just enough RFB 3.8 for remote-smoke.sh: no
# password, one 320x200 frame, red on the left and blue on the right.
# usage: fake-vnc.py <port>
import socket, struct, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(1)
c, _ = s.accept()
c.sendall(b"RFB 003.008\n"); c.recv(12)
c.sendall(b"\x01\x01"); c.recv(1)            # security: none
c.sendall(struct.pack(">I", 0)); c.recv(1)    # result ok, ClientInit
W, H = 320, 200
c.sendall(struct.pack(">HH", W, H) + b"\0" * 16 + struct.pack(">I", 4) + b"fake")
time.sleep(0.5)
c.recv(4096)
px = bytearray()
for y in range(H):
    for x in range(W):
        px += bytes([0, 0, 255, 0]) if x < W // 2 else bytes([255, 0, 0, 0])  # BGRX: red | blue
c.sendall(b"\x00\x00" + struct.pack(">H", 1) + struct.pack(">HHHHi", 0, 0, W, H, 0) + bytes(px))
time.sleep(6)
