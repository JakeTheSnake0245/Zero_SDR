# Building the Zero_SDR firmware image

The flashable image (`firmware/zero_sdr_slim.itb`) is a U-Boot FIT containing four parts:
FPGA bitstream, Linux kernel, device tree, and a ramdisk (rootfs). It is assembled with `mkimage`
from [`tezuka_slim.its`](tezuka_slim.its).

This repo ships the **text** inputs (device-tree sources, the `.its` recipe) and the source for the
board daemon and host module. The large binary inputs (kernel image, FPGA `.bin`, upstream rootfs)
come from the upstream Tezuka/ADI build and are not redistributed here — get them from
[F5OEO/plutosdr-fw](https://github.com/F5OEO/plutosdr-fw).

## 1. Kernel
Base: `analogdevicesinc/linux` 6.12 (`adi-6.12.y`) + the Tezuka Nano patch set, plus these upstream
DMABUF memory-leak backports (without them a low-latency run leaks ~2×128 B/cycle → OOM in ~11 min):

- IIO DMABUF: `a09399935508`, `f25ec4627d93`, `8662e56c31cf`
- FunctionFS DMABUF: `2796646f6d89`, `baa6b6068a3f`, `621707dc67c9`

Kernel config notes:
- `CONFIG_IIO_DMA_BUF_MMAP_LEGACY=n` (the legacy mmap block ops are mutually exclusive with the
  attach/enqueue_dmabuf path this uses)
- `CONFIG_DMABUF_HEAPS=y`, `CONFIG_DMABUF_HEAPS_CMA=y`

## 2. Device tree
Two changes from the stock Nano DTB (see [`fdt_slim.dts`](fdt_slim.dts), and
[`fdt_ro.dts`](fdt_ro.dts) for just the read-only-partition variant):

1. All four QSPI partitions marked `read-only` — the running system cannot corrupt flash, and it
   avoids the kernel issuing a 4-byte/EAR access past the 16 MB line.
2. `usb@e0002000`: `dr_mode = "otg"` → **`"peripheral"`**. Stock OTG mode picks host/peripheral from
   the ULPI PHY ID pin; powering the board from the UART-side USB-C makes the pin read grounded, so
   Linux comes up as a USB *host* and the data port is dead. Forcing peripheral makes the board a USB
   gadget regardless of which port is powered first. (U-Boot's DFU configures USB itself, so recovery
   is unaffected.)

## 3. DDR fix (U-Boot env, not the FIT)
Separately from the image, this unit needed a DDR IOB slew adjustment to stop memory bit-errors
under load. It lives in the U-Boot `preboot` variable (persists across firmware flashes because the
flash step only re-saves the env, it does not erase it):

```
preboot=mw f8000008 df0d; mw f8000b5c 00000518; mw f8000b60 0004861c; mw f8000b64 0004861c; <rest of stock preboot>
```

`f8000008=df0d` unlocks the SLCR; `f8000b5c/b60/b64` set the DDRIOB drive/slew. **Never** write the
SLCR lock key `0x767b` from Linux — it breaks the SPI clock gating and `reboot`.

## 4. Rootfs
Slimmed from the Tezuka rootfs: removed the on-device web SDR (maia-httpd), the config-drive image,
and unused libraries; kept the `iiod` dependency closure. Added `/usr/bin/zcd` (built from
[`../src/zcd.c`](../src/zcd.c)) and patched `S23udc` to create the `ffs.zc` FunctionFS function and
start `zcd -u` (TX unlocked) alongside `iiod`, then bind the UDC. The mass-storage/config-drive init
is disabled.

Keep the final FIT **under 16 MB** so `0x200000 + size` stays below the QSPI 16 MB boundary.

## 5. Assemble
```sh
mkimage -f tezuka_slim.its zero_sdr_slim.itb
```
(`.its` references `fdt_slim.dtb`, `img1.bin` (FPGA), `kernel_zc2.gz`, `slim_ramdisk.xz` in the
build directory.)
