# Phase 1 Baseline — HamGeek PlutoSDR Nano, stock firmware

Unit serial 03df643293c30f3a21d76c3a9bdb172239 · measured 2026-10-23 · room ambient · host: macOS 26, Apple Silicon, libiio 0.26.
Firmware as shipped: v0.39-dirty, kernel 6.1.0-271883-gf3da30df6004-dirty, U-Boot PlutoSDR v0.20-…-g90401ce9ce, FPGA dev_prj_2018_r1-2241-g065c8-dirty.
All tests are read-only or RAM-only. No flash or env writes. Raw data in `logs/`, method in `NOTES.md`.

## Platform

| Item | Value | Source |
|---|---|---|
| RF chip | AD9363BBCZ (marking), driver `ad9363a`, reg 0x037=0x0A (AD936x rev 2) | owner, IIO debugfs |
| CPU | 2× Cortex-A9 present, **1 online** (`maxcpus=1` in U-Boot env) @ 666.67 MHz (ARM PLL 1333.33) | /proc/cmdline, clk_summary |
| DDR | DDR3, **16-bit bus**, 512 MB, DDR3x 533.33 MHz (DDR3-1066), DDR2x 355.56; 256 MB CMA reserved | DDRC 0xF8006000=0x85, clk_summary, dmesg |
| DDR IOB slew | ADDR 0xF8000B5C=**0x0018C61C**; DATA/DIFF/CLOCK 0xF8000B60/64/68=**0x00F9861C** (Xilinx ps7_init defaults) | devmem (U-Boot + Linux, identical) |
| PL clocks | FCLK0 100 MHz, FCLK1 200 MHz, FCLK2/3 41.67 MHz; AD936x ref 40 MHz | clk_summary |
| FPGA cores | axi_ad9361 RX v0x000A0300, TX v0x00090262; 1R1T | debugfs |
| Flash | W25Q256 (ef 40 19), 3-byte addressing + EAR banking in Linux | spi-nor debugfs |
| USB | ChipIdea OTG + USB3320 ULPI, HS. **Gadget mode only if the board powers up from the Slave port** | OTGSC/PORTSC, dmesg |

## Memory integrity — stock DDR settings

| Run | Load | Duration | memtester loops | FAILURE words | Events (runs) | per loop |
|---|---|---|---|---|---|---|
| A | memtester 128M only | 10 min | ~2.3 | **44** | 7 | ~19 |
| A2 | memtester 128M only (RX stream failed to open) | 10 min | ~3.3 | **116** | — | ~35 |
| B | memtester 128M + RX 10 MS/s ci16 over USB (7.92 MS/s delivered) | 10 min | ~2.3 | **112** | 21 | ~48 |

- Failure type: **lost writes**. The bad word always holds the previous pass's pattern. Runs of 4 or 8 words (16 B = one x16 BL8 burst; 32 B = one cache line).
- Fingerprint: events cluster at a few physical in-page offsets (0xd54 in 9 of 28 events; 0x538, 0x558, 0xd34 …), mostly alternating-bit patterns. DDRC column mapping not yet decoded (Phase 2).
- Single core only (stock). The "two-core" stress in the plan is not possible without changing `maxcpus`.

## System stability under streaming (stock)

| # | Time | Load | Outcome |
|---|---|---|---|
| 1 | 21:09 | RX 8 MS/s ci16, IIO over USB (planned 600 s) | **Hang at ~150–180 s → watchdog reset** (REBOOT_STATUS gained SWDT_RST: 0x00480000 → 0x00490000) |
| 2 | ~21:26 | RX 4 MS/s, IIO over NCM network | **Hang within ~60 s → reset** (reason bits sticky; probably watchdog) |
| 3 | ~21:40 | RX 5 MS/s, IIO over NCM network | **Hang at start → reset** |
| — | | RX 180 s @ 8 MS/s USB; 600 s @ 10 MS/s USB + memtester; iperf3 2×30 s | completed |

**3 hangs in ≈2.5 h of streaming.** Flash re-verified after every reset: unchanged.

## RX streaming — IIO over USB (libiio usb backend, ci16, 2^18-sample buffers)

Drops = RX core DMA_OVF (0x88 bit 2), polled and cleared every 0.25 s during streaming.

| fs (MS/s) | delivered (MS/s) | MB/s | in-run overflow intervals | board CPU |
|---|---|---|---|---|
| 2.083 | 2.087 | 8.35 | 0 / 60 s | 6.4 % |
| 4 | 4.007 | 16.0 | 0 | 12 % |
| 5 | 5.008 | 20.0 | 0 | 15 % |
| 6 | 6.009 | 24.0 | 0 | 18 % |
| 7 | 7.008 | 28.0 | 0 | 21 % |
| **8** | **8.008** | **32.0** | **0** (also 0 over 180 s) | 23 % |
| 8.5 | 8.345 | 33.4 | every poll | — |
| 9 | 8.367 | 33.5 | every poll | — |
| 10 | 8.41 | 33.6 | every poll | 24 % |

**Max drop-free RX = 8.0 MS/s ci16. Ceiling ≈ 33.5 MB/s at only ~24 % board CPU.**

## RX streaming — IIO over USB network (CDC-NCM, runtime gadget swap; iiod TCP)

| fs (MS/s) | delivered | Mbit/s | in-run overflows | board CPU |
|---|---|---|---|---|
| 2.083 | 2.087 | 66.8 | 0 | 24.7 % |
| 3 | 3.006 | 96.2 | 0 | 34.9 % |
| 4 | 4.008 | 128.2 | 0 | 46.5 % |
| **4.5** | **4.508** | **144.2** | **0** | 52.2 % |
| 5 | — | — | — | board hung at start |

## USB networking — iperf3 3.22 over CDC-NCM (30 s each, TCP)

| Direction | Mbit/s | MB/s | Retransmits | Board CPU |
|---|---|---|---|---|
| Mac → board | 263.7 | 32.96 | 0 | 71 % |
| **Board → Mac** | **313.4** | **39.17** | 0 | **99.6 %** |

**Key observation:** board → host TCP reaches **39.2 MB/s** through the same USB controller where IIO-over-USB stops at **33.5 MB/s with 24 % CPU**.
So the IIO/USB ceiling is in the iiod/FunctionFS transfer path (request size, queue depth, copy), **not** in the USB link or controller. → Phase 3 / Phase 6.

## Thermal (die sensors; no heatsink probe)

| Condition | AD9363 | Zynq (XADC) |
|---|---|---|
| Idle, Oct 20 | 28.1 °C | 44.4 °C |
| memtester 10 min (A) | ≤ 38.6 °C | ≤ **61.3 °C** |
| RX 10 MS/s + memtester 10 min (B) | ≤ 34.2 °C | ≤ 54.5 °C |
| RX 8 MS/s | ~31 °C | 48.8–50.2 °C |

**Agreed limit: stop at Zynq ≥ 75 °C or AD9363 ≥ 80 °C.** Headroom is about 14 °C at the worst case seen.

## Baseline takeaways
1. **The stock board corrupts memory (lost writes) even with no DMA**, and **hangs under sustained streaming** (3× in 2.5 h). These are the two problems Phase 2 targets.
2. **The USB ceiling is a software path problem.** IIO 33.5 MB/s vs TCP 39.2 MB/s on the same link, with CPU headroom left over in the IIO case.
3. **Only one of two cores is in use** (`maxcpus=1`).
