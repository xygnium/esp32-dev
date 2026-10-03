#!/usr/bin/env python3
"""Listen for the ambient logger's pushes and answer each one.

Usage:
    ./udp_listener.py              # listen on UDP 8080
    ./udp_listener.py --port 9000

Prints each message with this host's local arrival time and the sender, and
replies "ack <UTC time>" so the logger can later check its clock against
this host. Stop with Ctrl-C.
"""

import argparse
import datetime
import socket


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    print(f"listening on UDP {args.port}", flush=True)

    while True:
        data, addr = sock.recvfrom(1024)
        now = datetime.datetime.now()
        text = data.decode("ascii", errors="replace")
        print(f"{now:%H:%M:%S} {addr[0]}:{addr[1]} {text}", flush=True)
        utc = datetime.datetime.now(datetime.timezone.utc)
        sock.sendto(f"ack {utc:%Y-%m-%dT%H:%M:%SZ}".encode(), addr)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
