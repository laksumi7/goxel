#!/usr/bin/env python3
"""AMFM #532 A3-P0 proof client: talk to one LAB editor bridge.

    python labctl.py [-p 47534] ping|state|set x y z [r g b]|del x y z|undo|redo|help
"""
import socket
import sys


def main():
    argv = sys.argv[1:]
    port = 47534
    if argv[:1] == ["-p"]:
        port = int(argv[1])
        argv = argv[2:]
    cmd = " ".join(argv) or "ping"
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.sendall((cmd + "\n").encode())
        buf = b""
        while True:
            try:
                d = s.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
            if buf.endswith(b"\nok\n") or b"\nok\n" in buf:
                break
        sys.stdout.write(buf.decode(errors="replace"))


main()
