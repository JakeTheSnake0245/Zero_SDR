#!/bin/bash
# RAM-boot a FIT image through the stock U-Boot (DFU to RAM; flash untouched). Every step is checked; stops on failure.
# usage: tools/ramboot.sh IMAGE.itb EXPECTED_UNAME_SUBSTRING
# After boot: verifies the kernel build string, locks TX down (-89.75 dB, LO off), checks all mtd partitions are read-only.
set -u
set -o pipefail   # checks pipe through tr: without this a failed step looks like success
IMG=$1; EXPECT=$2
cd "$(dirname "$0")/.."
U() { python3 -I tools/uart.py --log logs/ramboot.log "$@" | LC_ALL=C tr -d '\r'; }
die() { echo "RAMBOOT FAILED: $*"; exit 1; }

[ -f "$IMG" ] || die "no image $IMG"
echo "image $(md5 -q "$IMG")  $IMG"

# 1. get to a shell (log in if at a login prompt)
out=$(U --send "" --wait 2 || true)
if echo "$out" | grep -q "login:"; then U --send root --wait 2 >/dev/null; U --send analog --wait 4 >/dev/null; fi
o=$(U --send "echo SHELL_OK" --wait 2); echo "$o" | grep -q "^SHELL_OK" || die "no shell prompt"

# 2-3. reboot and stop at the U-Boot prompt (stop zcd first: an FFS function with live DMABUF attachments hangs the UDC unbind in the init scripts)
U --send "killall zcd zcstream 2>/dev/null; sleep 2; echo ZCD_STOPPED" --wait 4 >/dev/null
U --send reboot --wait 0.3 >/dev/null
U --spam " " --wait 60 --until "Pluto> *$" >/dev/null || die "did not reach the Pluto> prompt"

# 4. DFU RAM mode, wait for the device
U --send "run ramboot_verbose" --wait 2 >/dev/null
for _ in $(seq 1 30); do l=$(dfu-util -l 2>/dev/null); echo "$l" | grep -q 'name="firmware.dfu"' && break; sleep 0.5; done
echo "$l" | grep -q 'name="firmware.dfu"' || die "DFU RAM device never appeared"

# 5-6. download to RAM, detach
dl=$(dfu-util -d 0456:b674 -a firmware.dfu -D "$IMG" 2>&1)   # capture first: grep -q + pipefail = SIGPIPE false failure
echo "$dl" | grep -q "Download done" || die "DFU download failed"
dfu-util -d 0456:b674 -a firmware.dfu -e >/dev/null 2>&1

# 7. wait for login, log in
U --wait 150 --until "login:" >/dev/null || die "kernel did not reach a login prompt"
U --send root --wait 2 >/dev/null; U --send analog --wait 4 >/dev/null

# 8-10. verify kernel, lock TX, check flash read-only
v=$(U --send "uname -v" --wait 2 | grep -a "SMP")
echo "$v" | grep -q "$EXPECT" || die "unexpected kernel: $v"
U --send 'P=/sys/bus/iio/devices/iio:device0; echo -89.75 > $P/out_voltage0_hardwaregain; echo 1 > $P/out_altvoltage1_TX_LO_powerdown' --wait 2 >/dev/null
tx=$(U --send 'cat /sys/bus/iio/devices/iio:device0/out_altvoltage1_TX_LO_powerdown' --wait 2 | grep -aE '^[01]$')
[ "$tx" = "1" ] || die "TX LO not powered down"
ro=$(U --send 'for m in 0 1 2 3; do cat /sys/class/mtd/mtd$m/flags; done | tr "\n" " "; echo' --wait 2 | grep -aE '^0x')
[ "$ro" = "0x800 0x800 0x800 0x800 " ] || die "flash not read-only: $ro"
echo "RAMBOOT OK: $v | TX locked | mtd RO"
