#!/usr/bin/env python3
"""Minimal UART helper for the Nano console (115200 8N1).

usage: uart.py [--send TEXT]... [--wait SECONDS] [--until REGEX] [--log FILE]
Each --send is written followed by CR; output is printed and appended to the log.
"""
import argparse, glob, re, sys, time
import serial

p = argparse.ArgumentParser()
p.add_argument("--port", default=(sorted(glob.glob("/dev/cu.wchusbserial*")) or ["/dev/cu.wchusbserial10"])[0])
p.add_argument("--send", action="append", default=[])
p.add_argument("--wait", type=float, default=3.0)
p.add_argument("--until", default=None)
p.add_argument("--log", default="logs/uart.log")
p.add_argument("--spam", default=None, help="send this string repeatedly until --until matches")
a = p.parse_args()

s = serial.Serial(a.port, 115200, timeout=0.1)
log = open(a.log, "ab")
log.write(f"\n===== {time.strftime('%Y-%m-%d %H:%M:%S')} send={a.send} =====\n".encode())
buf = b""

def pump(deadline, pat=None, spam=None):
    global buf
    while time.time() < deadline:
        if spam:
            s.write(spam.encode())
        d = s.read(4096)
        if d:
            buf += d
            log.write(d); log.flush()
            sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
        if pat and re.search(pat, buf.decode(errors="replace")):
            return True
    return False

for cmd in a.send:
    buf = b""
    s.write((cmd + "\r").encode())
    pump(time.time() + a.wait, a.until)
if not a.send:
    ok = pump(time.time() + a.wait, a.until, a.spam)
    sys.exit(0 if (ok or not a.until) else 2)
