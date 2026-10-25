# Phase 2 — DDR stability fix (COMPLETE 2026-10-24)

Unit serial 03df643293c30f3a21d76c3a9bdb172239, stock v0.39-dirty, room ambient, die sensors only.
Tests ran with the fix applied at runtime; it was then made persistent via U-Boot `preboot` (owner-approved 2026-10-24, see below).

## The fix
Runtime, in Linux (devmem) or U-Boot (mw):

| Register | Stock | Fix | Decoded change |
|---|---|---|---|
| SLCR_UNLOCK 0xF8000008 | — | 0xDF0D | unlock only (**never write the lock key 0x767B under Linux**) |
| DDRIOB_DRIVE_SLEW_ADDR 0xF8000B5C | 0x0018C61C | **0x00000518** | drive p28/n12 → p24/n10, slew p3/n3 → p0/n0 |
| DDRIOB_DRIVE_SLEW_DATA 0xF8000B60 | 0x00F9861C | 0x0004861C | slew p6 → 18, n31 → 0 (drive unchanged) |
| DDRIOB_DRIVE_SLEW_DIFF 0xF8000B64 | 0x00F9861C | 0x0004861C | same as DATA |
| DDRIOB_DRIVE_SLEW_CLOCK 0xF8000B68 | 0x00F9861C | unchanged | — |

## Before / after (identical protocols)

| Test | Stock | Fix (ADDR+DATA+DIFF) |
|---|---|---|
| A: memtester 128M, 10 min, no DMA | 44 / 116 failures (2 runs) | **0** |
| B: memtester + RX 10 MS/s USB DMA, 10 min | 112 failures | **0** |
| memtester 30 min | — | **0** |
| RX 8 MS/s ci16 USB streaming | **hang + watchdog reset at ~150 s** (1 of 3 long runs) | **30 min clean, 0 overflows, no reset** |
| 60 min streaming soak 8 MS/s | — | **clean: 28.8 G samples, 0 overflows, no reset** |
| Two-core memtester (CPU1 online), 10 min | (pmp: >1 error/s) | **0** |
| 2 cores + 2×memtester + RX 10 MS/s, 30 min | — | **0 failures, no reset** |
| IIO over NCM network (1 core / 2 cores) | hung at 4–5 MS/s | **4.5–6 MS/s, 0 overflows, both** |
| iperf3 board→host (1 core / 2 cores) | 313 Mbit | 318 / 300 Mbit |
| Uptime without reset | 3 watchdog resets in ~2.5 h | **10 h 31 min, 0 resets** |
| Bandwidth memcpy / memset / read | 257 / ~2000 / 378 MB/s | **identical** |

## Which part matters (group sweep, memtester 10 min)

| Config | ADDR | DATA/DIFF | Result |
|---|---|---|---|
| Stock | stock | stock | 44–116 failures |
| X | **fix** | stock | **0 failures** |
| Y | stock | **fix** | **40 failures in ~4.5 min**, USB link dropped |
| Full | fix | fix | 0 failures |

**The address/command group is the critical one.**

## Physical fingerprint: REPLICATED (prior art: pimpmypluto docs/ARCHITECTURE.md, 2026-09-20)
- Lost writes come in 16 B / 32 B units (one x16 BL8 burst / one cache line). The stale previous value remains.
- Failure events cluster on the same physical in-page offsets across independent runs: **0xd54** (13 of 39 events across
  Phase 1 A/B and group Y), 0x558 ×5, 0x538 ×3, 0xd34 ×3, …, mostly alternating-bit patterns.
- Group Y (only ADDR at stock): **11/11 events on offsets already seen in Phase 1** (chance ≈ 1.7 % each).
- pimpmypluto already showed this in more depth: lost 16 B bursts at columns with A9=1, A5=1, A6=0, a partner burst 0x100 B away
  (column bit **A7 mis-sampled**), fixed by DDRIOB_DRIVE_SLEW_ADDR=0x00000518 (slew sweep 0/0 clean, 3/3 fails, 6/6 kills the board;
  drive window P 0x13–0x1E). Our hottest offsets (0xd54, 0x558, 0x538, 0xd34 → columns 0x2AA, 0x2AC, 0x29C, 0x29A) all have **A9=1, A7=1, A6=0**; A5 is mixed (1,1,0,0), so their A5=1 condition does not hold strictly here. **Our result = independent replication.**
- What we add: (1) second unit, **stock vendor firmware/kernel**; (2) measured stock streaming hangs, removed by the runtime fix;
  (3) group sweep: the **DATA/DIFF change (0x0004861C) neither fixes nor is needed**, ADDR alone is sufficient (they did not sweep data).

## Side findings
- **Overnight USB drops (Mac lost the device with 2 cores online) did NOT reproduce** in a fresh boot with the Mac kept awake (2-core NCM leg clean).
  Most likely host-side (macOS sleep/DarkWake cycling all night), not the board. Board-side: VBUS stays "valid" when powered from the UART side,
  so the gadget never sees an unplug; recovery = unbind ≥10 s + rebind, or a cold re-power.
- **SLCR lock pitfall:** re-locking the SLCR under Linux silently drops the kernel's clock-gate writes (SPI0 stuck off → AD9363 SPI
  timeouts) and its soft-reset write (`reboot` hangs). Recovery without a reboot: unlock, then set SPI_CLK_CTRL 0x601 and APER_CLK_CTRL bit 14.
- **iiod 0.26 can wedge** if a new USB IIO session opens within ~1 s of the previous one closing ("Failed to reset pipes").
  Fix: `/etc/init.d/S23udc stop; start`. Tools now pause between sessions.

## Persistence — DONE (owner-approved 2026-10-24)
Executed exactly as below. Fresh U-Boot session, full printenv diff = only `preboot` changed. After `saveenv` + `reset` there was no bad CRC,
and Linux shows the fix with no manual action, including after a cold power cycle. **mtd1 md5 c6d1432b… → c7c176dcf044f841b5802facb52e4856.**
Other partitions unchanged. Copy: backup/uboot-env_with_ddrfix_2026-10-24.bin.

### As proposed and executed
Env-only change, never the FSBL. Prepend the fix to U-Boot `preboot`, in a **fresh** U-Boot session (no other setenv, or they would be saved too):

```
Pluto> mw f8000008 df0d; mw f8000b5c 00000518; mw f8000b60 0004861c; mw f8000b64 0004861c   # fix first, so the env is staged in fixed RAM
Pluto> setenv preboot 'mw f8000008 df0d; mw f8000b5c 00000518; mw f8000b60 0004861c; mw f8000b64 0004861c; if test $modeboot = sdboot && env run sd_uEnvtxt_existence_test; then if env run loadbootenv; then env run importbootenv; fi; fi; '
Pluto> saveenv
```
- **Target:** mtd1 / uboot-env.dfu (0x100000–0x120000) only.
- **Backup:** backup/dfu_r1/uboot-env.dfu, md5 c6d1432ba10c25860bcf3974f3166001 (and the printenv text).
- **Verify:** reset, check that `printenv preboot` shows the new line, `md.l f8000b5c 1` = 00000518, Linux devmem = 0x518. Read the env back over DFU and keep its new md5.
- **Rollback:**
  - U-Boot: `setenv preboot '<stock line above without the mw prefix>'; saveenv`.
  - Or DFU: restore uboot-env.dfu (RESTORE.md rung 1) → md5 c6d1432b….
- **Residual risk:** FSBL + early U-Boot still run with stock slew for well under a second (no DMA, little DDR traffic).
