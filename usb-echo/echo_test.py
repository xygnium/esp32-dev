#!/usr/bin/env python3
# Host side of usb-echo. Sends numbered test lines and checks each echo.
# Usage: echo_test.py [--port P] [--lines N] [--gap S]
#   --gap is the quiet time after each echo: 0 tests steady traffic, 30
#   repeats the quiet spells that came before the 64-byte junk.
# For each line: OK if exactly that line comes back, else BAD with a hex dump.
#   Bytes that arrive when no echo is due are counted as extra.
# Does not reset the board. Stop any monitor first.
# Needs pyserial: in the VM use the ESP-IDF Python
#   (~/dev/esp32/tools/python_env/idf6.1_py3.14_env/bin/python).
import argparse
import time
import zlib

import serial

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="/dev/ttyUSB0")
ap.add_argument("--lines", type=int, default=50)
ap.add_argument("--gap", type=float, default=0.0)
a = ap.parse_args()

PATTERN = bytes(range(0x21, 0x7F))[:60]


def line(seq):
    body = b"%06d " % seq + PATTERN
    return body + b" %08x\n" % zlib.crc32(body)


def hexdump(data, limit=64):
    for i in range(0, min(len(data), limit), 16):
        row = data[i:i + 16]
        print("    %04x  %-47s  %s" % (i, " ".join("%02x" % b for b in row),
                                     "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in row)))
    if len(data) > limit:
        print("    ... %d bytes in all" % len(data))


def read_for(s, seconds):
    end, got = time.monotonic() + seconds, b""
    while time.monotonic() < end:
        got += s.read(4096)
    return got


s = serial.Serial(a.port, 115200, timeout=0.05)
pre = read_for(s, 1.0)                  # whatever was queued before the test
print("before the test: %d bytes waiting" % len(pre))
if pre:
    hexdump(pre)

ok = bad = missing = extra = 0
start = time.monotonic()
for seq in range(1, a.lines + 1):
    sent = line(seq)
    s.write(sent)
    got, end = b"", time.monotonic() + 1.0
    while len(got) < len(sent) and time.monotonic() < end:
        got += s.read(len(sent) - len(got))
    got += read_for(s, 0.1)             # anything beyond the echo
    if got == sent:
        ok += 1
    elif not got:
        missing += 1
        print("%s  line %d: no echo" % (time.strftime("%H:%M:%S"), seq))
    else:
        bad += 1
        print("%s  line %d: BAD, sent %d bytes, got %d" % (time.strftime("%H:%M:%S"), seq, len(sent), len(got)))
        print("  sent:")
        hexdump(sent)
        print("  got:")
        hexdump(got)
    if a.gap:
        quiet = read_for(s, a.gap)
        if quiet:
            extra += len(quiet)
            print("%s  %d extra bytes during the %.0f s gap after line %d" % (
                time.strftime("%H:%M:%S"), len(quiet), a.gap, seq))
            hexdump(quiet)

took = time.monotonic() - start
print("\n%d lines in %.0f s: %d ok, %d bad, %d with no echo, %d extra bytes in gaps"
      % (a.lines, took, ok, bad, missing, extra))
