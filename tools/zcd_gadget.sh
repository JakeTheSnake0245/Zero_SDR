#!/bin/sh
# RUNTIME ONLY: add the zero-copy FunctionFS function (ffs.zc) to the composite gadget and start zcd.  Undo: reboot.
# usage: sh zcd_gadget.sh   (zcd starts idle; streams/params come from the host via ep0)
G=/sys/kernel/config/usb_gadget/composite_gadget

killall zcstream zcd 2>/dev/null; sleep 1
mkdir -p $G/functions/ffs.zc /dev/zc
mountpoint -q /dev/zc || mount -t functionfs zc /dev/zc
/tmp/zcd $ZCD_ARGS /dev/zc > /tmp/zcd.log 2>&1 &
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q "descriptors written" /tmp/zcd.log && break; sleep 0.5; done
if ! grep -q "descriptors written" /tmp/zcd.log; then
  echo "zcd did not accept descriptors; gadget left untouched"; cat /tmp/zcd.log; exit 1
fi
echo "" > $G/UDC 2>/dev/null   # may already be unbound (killing the daemon unbinds the gadget)
[ -e $G/configs/c.1/ffs.zc ] || ln -s $G/functions/ffs.zc $G/configs/c.1/ffs.zc
sleep 1
echo ci_hdrc.0 > $G/UDC
sleep 2
echo "UDC $(cat /sys/class/udc/ci_hdrc.0/state)"; tail -3 /tmp/zcd.log
