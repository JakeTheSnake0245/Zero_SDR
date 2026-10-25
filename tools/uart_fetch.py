#!/usr/bin/env python3
"""Pull a file off the board over the console: runs `uuencode -m <remote>` and
decodes the base64 block on the host.

usage: uart_fetch.py REMOTE_PATH LOCAL_PATH
"""
import base64, glob, re, sys, time
import serial

remote, local = sys.argv[1], sys.argv[2]
port = sorted(glob.glob("/dev/cu.wchusbserial*"))[0]
s = serial.Serial(port, 115200, timeout=0.5)
s.reset_input_buffer()
s.write(f"uuencode -m {remote} x; echo XFER_\"\"END\r".encode())

buf = bytearray()
t0 = last = time.time()
while True:
    d = s.read(65536)
    if d:
        buf += d
        last = time.time()
        if b"\nXFER_END" in buf:
            break
    elif time.time() - last > 30:
        sys.exit(f"stalled after {len(buf)} bytes")
    if len(buf) and int(time.time() - t0) % 60 == 0 and d:
        print(f"{int(time.time() - t0)}s {len(buf)} bytes", flush=True)

text = buf.decode(errors="replace").replace("\r", "")
m = re.search(r"begin-base64 \d+ x\n(.*?)\n====\n", text, re.S)
if not m:
    open(local + ".raw", "wb").write(buf)
    sys.exit("no base64 block found; raw capture saved")
data = base64.b64decode("".join(m.group(1).split()))
open(local, "wb").write(data)
print(f"ok {len(data)} bytes in {int(time.time() - t0)}s -> {local}")
