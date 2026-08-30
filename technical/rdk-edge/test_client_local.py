#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本地假客户端: 连 test_rdk_stream.exe 收流验证协议 + 回发事件"""
import socket, struct, sys, time

MAGIC_FRAME = b"INFR"
MAGIC_EVENT = b"INEV"

def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)

def main():
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
    sock = socket.create_connection(("127.0.0.1", 9999), timeout=5)
    sock.settimeout(5)
    print("[client] connected")
    t0 = time.time()
    frames = 0
    while time.time() - t0 < dur:
        hdr = recv_exact(sock, 16)
        if hdr is None:
            print("[client] server closed")
            break
        magic, length, ts = struct.unpack("<4sIQ", hdr)
        assert magic == MAGIC_FRAME, magic
        payload = recv_exact(sock, length)
        assert payload is not None and len(payload) == length
        frames += 1
        if frames % 20 == 0:  # 每20帧回发一个事件
            evt = b'{"type":"stats","frames":%d}' % frames
            sock.sendall(MAGIC_EVENT + struct.pack("<I", len(evt)) + evt)
    print("[client] received %d frames in %.1fs (%.1f fps)" % (frames, dur, frames/dur))
    sock.close()

if __name__ == "__main__":
    main()
