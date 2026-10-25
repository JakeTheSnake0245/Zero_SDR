#!/usr/bin/env python3
"""Push a host file to the board's RAM over the console (base64 → busybox uudecode).

usage: uart_put.py LOCAL_PATH REMOTE_PATH
Prints the board-side md5 so it can be compared with the host md5.
"""
import base64, glob, hashlib, sys, time
import serial

local, remote = sys.argv[1], sys.argv[2]
data = open(local, "rb").read()
b64 = base64.encodebytes(data).decode()
port = sorted(glob.glob("/dev/cu.wchusbserial*"))[0]
s = serial.Serial(port, 115200, timeout=0.05)


def drain(t):
    end, out = time.time() + t, b""
    while time.time() < end:
        out += s.read(4096)
    return out


s.write(b"stty -echo; cat > /tmp/.put.b64\r")
drain(0.5)
s.write(b"begin-base64 644 x\n")
t0 = time.time()
for i in range(0, len(b64), 4096):
    s.write(b64[i:i + 4096].encode())
    s.flush()
    drain(0.01)
s.write(b"====\n")
s.write(b"\x04")
drain(1.0)
# decode to a temp file and rename: works even if the old binary is running (no ETXTBSY)
s.write(f'stty echo; uudecode -o {remote}.new /tmp/.put.b64 && rm /tmp/.put.b64 && chmod +x {remote}.new && mv -f {remote}.new {remote}; md5sum {remote}; echo PUT_""DONE\r'.encode())
out = drain(4).decode(errors="replace")
want = hashlib.md5(data).hexdigest()
ok = f"{want}  {remote}" in out.replace("\r", "")
print("host md5", want, f"({len(data)} B in {time.time() - t0:.0f}s)", "board md5 MATCH" if ok else "BOARD MD5 MISMATCH/UPLOAD FAILED")
sys.exit(0 if ok else 1)
