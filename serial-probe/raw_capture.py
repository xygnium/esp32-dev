#!/usr/bin/env python3
# Raw serial capture for the garbled-output fault. Stop the monitor first: the
#   port can only be opened once.
# Usage: raw_capture.py [port] [outfile]   (default /dev/ttyUSB0, /tmp/raw_capture.bin)
# Writes every byte received to outfile unchanged. On the console, prints each
#   read that holds a byte outside printable ASCII, tab, CR and LF as a hex dump,
#   with the time and the read's size, plus the 40 bytes received before it.
#   Ordinary reads print as one summary line per 30 s block.
# Opening the port may reset the board.
# Needs pyserial: in the VM use the ESP-IDF Python
#   (~/dev/esp32/tools/python_env/idf6.1_py3.14_env/bin/python); on an Ubuntu
#   host, sudo apt install python3-serial.
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/raw_capture.bin"
OK = set(range(0x20, 0x7F)) | {0x09, 0x0A, 0x0D}


def hexdump(data, base=0):
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        hx = " ".join(f"{b:02x}" for b in row)
        txt = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in row)
        print(f"    {base + i:04x}  {hx:<47}  {txt}")


s = serial.Serial(port, 115200, timeout=0.2)
print(f"capturing {port} to {out}, Ctrl+C to stop")
recent = b""
total = bad_reads = 0
block_bytes = block_reads = 0
last_rx = time.monotonic()
with open(out, "wb") as f:
    try:
        while True:
            data = s.read(4096)
            now = time.monotonic()
            if not data:
                if block_bytes and now - last_rx > 5:
                    print(f"{time.strftime('%H:%M:%S')}  block: {block_bytes} bytes "
                          f"in {block_reads} reads")
                    block_bytes = block_reads = 0
                continue
            gap = now - last_rx
            last_rx = now
            f.write(data)
            f.flush()
            total += len(data)
            block_bytes += len(data)
            block_reads += 1
            if any(b not in OK for b in data):
                bad_reads += 1
                print(f"{time.strftime('%H:%M:%S')}  BAD read #{bad_reads}: {len(data)} bytes, "
                      f"{gap:.1f} s after the previous read, offset {total - len(data)}")
                print("  before:")
                hexdump(recent)
                print("  read:")
                hexdump(data)
            recent = (recent + data)[-40:]
    except KeyboardInterrupt:
        pass
print(f"\n{total} bytes, {bad_reads} bad reads, saved to {out}")
