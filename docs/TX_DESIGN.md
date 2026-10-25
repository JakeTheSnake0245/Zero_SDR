# TX zero-copy design (draft, 2026-10-24) — nothing transmitted yet

Goal: TX as clean as the RX path. Host → USB → DAC with no CPU copy, no underflow, low and known latency,
and full duplex alongside RX. **Safety is designed in; nothing radiates during development.**

## Data path (mirror of RX)
```
host libusb async bulk OUT ─► FunctionFS ep (OUT) ── FUNCTIONFS_DMABUF_TRANSFER fills DMABUF (USB DMA)
                                                       │ poll(POLLIN): USB write fence done
                                                       ▼
                     IIO TX buffer (cf-ad9361-dds-core-lpc, iio:device2) ── IIO_BUFFER_DMABUF_ENQUEUE ─► axi-dmac ─► DAC
                                                       │ poll(POLLOUT): DAC DMA read done
                                                       └─► back to USB (ring of N DMABUFs, CMA heap)
```
- Same kernel (LEGACY off + CMA heap), same `zcstream` process, with a second endpoint (bulk OUT) on the same `0xFF/0x5A` interface.
- **CS8 TX = enable `voltage0` only** on the DDS core (confirmed in SoapyPlutoPAPR: "Tezuka TX CS8 output"); CS16 = both channels.
- Underrun detection: DAC core `DMA_STATUS` reg 0x88 bit 1 (UNF, W1C), polled like RX's OVF. Core base from the DT, not guessed.
- Pre-roll: start the DAC only after K buffers are queued; on host stall, either zero-fill (configurable) or report UNF.

## Bandwidth budget (USB 2.0 is half-duplex shared: ~44.3 MB/s total, IN + OUT)
| Mode | RX | TX | Total |
|---|---|---|---|
| RX only | 21 MS/s cs8 | — | 42 MB/s |
| TX only | — | ~21 MS/s cs8 / ~10 cs16 | 42 / 40 MB/s |
| Full duplex (symmetric) | ~10.5 MS/s cs8 | ~10.5 MS/s cs8 | ~42 MB/s |
Full duplex at 20 MHz each way does not fit; per-direction rates are user-selectable within the budget.

## Control plane (same interface, no second process fighting libusb exclusivity on macOS)
- Vendor control requests on ep0 (FunctionFS SETUP events) for RX/TX LO, rate, RF BW, gains, FIR, and start/stop.
  `zcstream` applies them via sysfs. Replies carry status, OVF/UNF counters and timestamps (sample counter).
- Timestamps: a 64-bit sample counter per buffer, for VITA-49 later and for TX scheduling.

## TX safety (enforced on the board, not just in the host app)
1. TX **disarmed by default**: TX LO powerdown=1, hardwaregain −89.75 dB at stream start.
2. **Arm** needs an explicit control request carrying a frequency inside an **owner-configured allow-list**, plus a gain at or below an **owner-set ceiling**.
3. Watchdog: if the host stops feeding TX, the board zeroes the output and disarms after T ms (no stuck carrier).
4. Development and testing (throughput, underrun, latency of the DMA path) run **disarmed**: DAC/DMA fully exercised, TX LO off, max attenuation → no RF.
5. RF-on tests only with an owner-approved setup (dummy load, or cabled loopback TX→≥30 dB→RX) on owner-approved frequencies.

## Test plan
| Step | RF? | What |
|---|---|---|
| T1 | no | TX zero-copy throughput & UNF, cs8/cs16, rate sweep, 60-min soak (disarmed) |
| T2 | no | Full-duplex budget sweep (RX+TX), OVF/UNF both sides (disarmed) |
| T3 | **yes (approval)** | Cabled loopback TX→≥30 dB→RX: tone/burst integrity, EVM, I/Q order, TX→RX round-trip latency |
| T4 | **yes (approval)** | Dummy load: 30-min TX soak for the deferred warm-temperature DDR test (Phase 2) |
| T5 | no | SoapySDR TX streams (CS8/CS16/CF32) in SDR++ / GNU Radio, disarmed then approved |
