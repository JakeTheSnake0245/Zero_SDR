# RESTORE.md — HamGeek PlutoSDR Nano, serial 03df643293c30f3a21d76c3a9bdb172239

Every write below is persistent. Run a restore step only after confirming that the board is really broken, and only
one rung of the ladder at a time. Do not jump straight to boot.dfu.

## 0. What "good" looks like (stock, as received)
- UART: `U-Boot PlutoSDR v0.20-PlutoSDR-00066-g90401ce9ce`, prompt `Pluto>`, model "Zynq Pluto SDR Board".
- Linux: v0.39-dirty, kernel 6.1.0-271883-gf3da30df6004-dirty, login root/analog.
- Known harmless boot noise: `mount: mounting mtd2 on /mnt/jffs2 failed` (factory defect; mtd2 holds PC-BIOS leftovers).

## 0b. Persistent change made (owner-approved 2026-10-24)
- U-Boot env `preboot` now starts with the DDR fix: `mw f8000008 df0d; mw f8000b5c 00000518; mw f8000b60 0004861c; mw f8000b64 0004861c; …`
- mtd1 (uboot-env) md5 **c7c176dcf044f841b5802facb52e4856** (copy: backup/uboot-env_with_ddrfix_2026-10-24.bin). Stock env: dfu_r1/uboot-env.dfu (c6d1432b…).
- **Rollback:** DFU rung 1 with `dfu_r1/uboot-env.dfu`, or at `Pluto>`: `setenv preboot 'if test $modeboot = sdboot && env run sd_uEnvtxt_existence_test; then if env run loadbootenv; then env run importbootenv; fi; fi; '; saveenv`.
- All other partitions unchanged (mtd0 e90b6285, mtd2 272a26c8, mtd3-low d1f73b33, upper e3e39afd).

## 1. Connection rule for this unit (important)
**Power up from the "Slave" USB-C port first** (USB-C ↔ USB-C straight to the host), then plug the UART port.
If the board powers up from the UART port, Linux boots as a USB *host* (OTG ID reads grounded) and the data port is
dead until a cold re-power. U-Boot DFU is not affected by this.

## 2. Backup set (all md5-verified, two independent DFU reads + on-device kernel md5)
Location 1: `~/Desktop/nano-sdr/backup/` (see `MANIFEST.md5`). Locations 2 and 3: _to be named by owner_.

| File | Flash range | Size | md5 | Verified against |
|---|---|---|---|---|
| dfu_r1/boot.dfu | 0x0000000–0x0100000 | 1,048,576 | e90b628510a66cc477847cffd3c8754d | mtd0, 2nd DFU read |
| dfu_r1/uboot-extra-env.dfu | 0x00FF000–0x0100000 | 4,096 | b2be550bc4b37c4f0cc1d98d7a70e958 | 2nd DFU read (not a valid env; random bytes) |
| dfu_r1/uboot-env.dfu | 0x0100000–0x0120000 | 131,072 | c6d1432ba10c25860bcf3974f3166001 | mtd1, 2nd DFU read |
| dfu_r1/spare.dfu | 0x0120000–0x0200000 | 917,504 | 272a26c81574b0cd05b6cdd0246724c5 | mtd2, 2nd DFU read |
| dfu_r1/firmware_lo.dfu | 0x0200000–0x1000000 | 14,680,064 | d1f73b3399d527b6b145e7d9ecafe572 | mtd3 lower, 2nd DFU read |
| flash_upper16M_0x1000000.bin | 0x1000000–0x2000000 | 16,777,216 | e3e39afdaaa2d797dcc9dab8dbf7d856 | on-device md5 (×2 dates) |
| **nano_full_32M_2026-10-23.bin** | 0x0000000–0x2000000 | 33,554,432 | **4b3a811a5323567b91ff523f75d62d61** | assembled; mtd3 part = 0cda1081… ✔ |
| uboot_printenv_stock_2026-10-20.txt | (text) | | 698114c1a1b62ada36d5fb2ba642049c | |

The stock firmware boot only reads 0x200000 + fit_size (0x900000). Nothing at or above 0x1000000 is needed to boot.

## 3. Safety rules for any restore
- **The stock DDR settings drop writes (Phase 1 Test A: 44 lost-write failures in 10 min of memtester).** Data staged in board RAM
  can be corrupted before it reaches flash. **Always read back every written region and compare md5 with the table.** On a mismatch, write again.
  If possible, apply the Phase 2 DDR slew fix at runtime (devmem) before any restore.
- **Never use the stock `run dfu_sf` script.** Its firmware.dfu alt spans 0x200000–0x2000000 (crosses 16 MB → bank-register writes),
  and it runs `env save` after a firmware.dfu transfer.
- **Never press the DFU button to enter DFU.** Stock `qspiboot` then runs `sf protect lock 0 100000` (flash status-register write).
- Never `saveenv`, `env default -a`, or `run jtagboot` (does `env default -a` + unlock).
- Only write regions below 0x1000000 from U-Boot/DFU. The upper 16 MB is restored only with an off-board programmer (rung 4).

## 4. Recovery ladder (one rung at a time; stop and re-assess after each)

### Rung 1 — DFU (board reaches the `Pluto>` prompt)
1. UART: `picocom -b 115200 /dev/cu.wchusbserial*`. Press a key during "Hit any key to stop autoboot".
2. At `Pluto>` (RAM-only; nothing saved):
   ```
   setenv dfu_alt_info "boot.dfu raw 0x0 0x100000;uboot-extra-env.dfu raw 0xFF000 0x1000;uboot-env.dfu raw 0x100000 0x20000;spare.dfu raw 0x120000 0xE0000;firmware_lo.dfu raw 0x200000 0xE00000"
   dfu 0 sf 0:0:40000000:0
   ```
3. Host: `dfu-util -l` must list exactly those 5 alts on 0456:b674.
4. Restore only what is broken, most-likely-broken first:
   ```
   dfu-util -d 0456:b674 -a firmware_lo.dfu -D backup/dfu_r1/firmware_lo.dfu     # Linux/FPGA image
   dfu-util -d 0456:b674 -a uboot-env.dfu   -D backup/dfu_r1/uboot-env.dfu       # U-Boot environment
   # boot.dfu (FSBL+U-Boot): ONLY if U-Boot itself is broken, and then you can't be at this prompt; see rung 3.
   ```
5. Verify by reading back (`-U` into a new file) and comparing md5 with the table. Ctrl-C at the console, then `reset`.

### Rung 2 — U-Boot serial (`loady` + `sf update`), no USB data
Slow (~115200 baud; firmware_lo ≈ 21 min). Same region limits as rung 1.
```
Pluto> sf probe 0:0 40000000 0
Pluto> loady 0x3000000            # host: send backup/dfu_r1/firmware_lo.dfu with YMODEM (e.g. picocom --send-cmd "sb -vv")
Pluto> crc32 0x3000000 0xE00000   # compare with `crc32` of the file on the host
Pluto> sf update 0x3000000 0x200000 0xE00000
```
Env: `loady 0x3000000` with uboot-env.dfu, then `sf update 0x3000000 0x100000 0x20000`.

### Rung 3 — JTAG (no U-Boot prompt at all)
- Pads: TBD (Phase 4 will map them). Adapter: Digilent HS2-compatible.
- Load an FSBL + U-Boot into RAM over JTAG, then use rung 1. Tezuka v0.3.24 ships `flash/jtag/{fsbl.elf,u-boot.elf,boot_fsbl_uboot.sh,tezuka.cfg}`
  for this board target. That U-Boot is Tezuka's: check its env behaviour on the UART before running any dfu command.
- Once at a prompt, restore `boot.dfu` with rung 1 (explicit owner approval required).

### Rung 4 — CH341A on the W25Q256 (board unpowered; last resort; also the only way to restore 0x1000000+)
```
flashrom -p ch341a_spi                               # identify: expect W25Q256 (JEDEC ef 40 19)
flashrom -p ch341a_spi -c W25Q256.V -r ch341_before.bin   # read twice, compare md5 → record what is on the chip now
flashrom -p ch341a_spi -c W25Q256.V -w backup/nano_full_32M_2026-10-23.bin   # unprotect/erase/program/verify
```
Expect md5 4b3a811a5323567b91ff523f75d62d61 on a readback. If in-circuit reads are unstable, hold the Zynq in reset (PS_POR_B low) or lift the chip.
Use 3.3 V CH341A only (many cheap CH341A boards put 5 V on the data lines; modify or use a level-shifted one).

---

## Section 1 — Flashed slim appliance (2026-10-25)

**Flash now holds the custom Option A "slim" appliance firmware** (was stock Pluto before).
- Firmware partition (SPI `0x200000`, DFU alt `firmware.dfu`) ← `firmware/zero_sdr_slim.itb`
  md5 `ac239452a90035d6522a53f0e6e88307`, 13,976,122 bytes (below the 16 MB EAR line).
  (Earlier v0.1.0 build `141a2cc9…` is the same image without the stable-serial fix.)
- Contents: DMABUF kernel `#3` + Tezuka FPGA + read-only-partition DTB w/ `dr_mode="peripheral"` (USB gadget forced, cable-order fix) + slim rootfs (`iiod` + `zcd -u` autostart, unlocked TX, stable ethaddr-derived serial).
- Env: `fit_size=0xD52A82`; DDR `preboot` fix intact; FSBL+U-Boot (`0x0–0x100000`) untouched.

### Rollback to stock firmware (DFU, reachable on cold boot regardless of cable order)
```
# at Pluto> prompt:  run dfu_sf
dfu-util -d 0456:b674 -a firmware.dfu -D backup/dfu_r1/firmware_lo.dfu   # md5 d1f73b3399d527b6b145e7d9ecafe572 (stock firmware, byte-verified r1==r2)
dfu-util -d 0456:b674 -a firmware.dfu -e                                 # auto-sets fit_size + env save
```
Backstops: `backup/dfu_r1/boot.dfu` (FSBL+U-Boot), `backup/uboot-env_with_ddrfix_2026-10-24.bin`, full dump `backup/nano_full_32M_2026-10-23.bin` (md5 4b3a811a…).
Never DFU-*upload* (read) the firmware.dfu alt: its length 0x1E00000 crosses 16 MB → EAR hazard.
