# Zero_SDR

A stability-and-throughput firmware for the **ADALM-PLUTO / PlutoSDR "Nano"** class of SDRs
(Xilinx Zynx-7010 + Analog Devices AD9361/AD9363, USB 2.0). It is built on top of the
[Analog Devices](https://github.com/analogdevicesinc/plutosdr-fw) and
[F5OEO "Tezuka"](https://github.com/F5OEO/plutosdr-fw) firmware, and adds a **zero-copy USB
streaming path** plus a handful of stability fixes so the radio can run near its real RF
bandwidth over USB 2.0 without choppy audio or dropped samples.

It is a single, flashable appliance image. It still presents the **standard PlutoSDR interface**
(so every existing tool keeps working), and *also* presents a second **zero-copy interface** that
a SoapySDR module (`driver=nanozc`) uses for high-rate streaming.

> Tested on one HamGeek PlutoSDR Nano (AD9363BBCZ, 512 MB DDR, 32 MB QSPI). Treat the numbers as
> "measured on that unit," not a spec. The original vendor firmware is included in
> [`firmware/stock/`](firmware/stock/) so you can always return to stock — see
> [Flashing](#flashing) and [`docs/RESTORE.md`](docs/RESTORE.md).

## Downloads

Prebuilt firmware, host driver, and the SDR++ plugin are on the
**[Releases page](../../releases)**. Install instructions: [`dist/README.md`](dist/README.md).

| File | What it is | Needed for |
|---|---|---|
| `zero_sdr_slim.itb` | the firmware (flash to the board) | everything |
| `libSoapyNanoZC.so` | host SoapySDR driver (`driver=nanozc`) | SCEPTRE **and** SDR++ |
| `soapy_source.dylib` | SoapySDR source plugin for SDR++ v1.3.0 (macOS arm64) | SDR++ only |

The host driver `libSoapyNanoZC.so` is prebuilt for **macOS arm64, Linux x86-64, and Linux ARM64**
(Linux builds target the SoapySDR 0.8 ABI). The SDR++ plugin is macOS-arm64 / SDR++ v1.3.0 only;
anything else builds from source ([`src/soapy/`](src/soapy/), [`docs/SDRPP_MACOS.md`](docs/SDRPP_MACOS.md)).

---

## What it actually does better

The one thing a Pluto is bad at out of the box is **moving IQ over USB 2.0 at high rates**. The
stock libiio USB path realistically sustains only a few MS/s before a real-time consumer (an SDR
GUI doing a waterfall) starts stuttering. Zero_SDR's zero-copy path removes the per-sample copies
and the in-band control traffic that cause that.

| Capability | Stock / typical Tezuka | **Zero_SDR** |
|---|---|---|
| Sustained RX over USB 2.0 (no drops, smooth to a real-time sink) | ~5–6 MS/s before choppiness | **20 MS/s, 0 dropped samples** |
| USB data path | libiio copies + in-band status | **DMABUF zero-copy** (dma-heap → IIO → FunctionFS), free-running host ring |
| Full duplex | fragile at high rates | 10 + 10 MS/s sustained, 0 errors (tuned buffers) |
| DDR memory stability | 44–116 bit-errors / 10 min on this unit | **0 errors** (DDR slew fix applied in U-Boot `preboot`) |
| Long low-latency runs | DMABUF fence leak → OOM in ~11 min | **+36 KB over a run** (upstream leak backports) |
| USB-C cable order | must power the "Slave" port first or the data port comes up dead (host mode) | **order-independent** (`dr_mode = "peripheral"`) |
| Tool compatibility | one interface | **both at once**: standard Pluto *and* zero-copy, same device |
| Flash safety | partitions writable at runtime | all MTD partitions **read-only** |

"Slightly better" is the honest framing: this is not a new radio stack, it is the Tezuka/ADI stack
with the USB streaming path rebuilt for throughput and a few stability potholes filled in. If you
only ever use a few MS/s, stock is fine. If you want to use most of the AD9363's ~20 MHz of
bandwidth live over USB, that is what this is for.

### How the zero-copy path works
- Board side: a small daemon (`zcd`, [`src/zcd.c`](src/zcd.c)) owns a FunctionFS USB function and
  wires the IIO DMABUF buffers straight to the USB endpoints — the same physical pages are DMA'd by
  the ADC/DAC core and by the USB controller, no CPU copy. Protocol in
  [`src/zcproto.h`](src/zcproto.h).
- Host side: a SoapySDR module ([`src/soapy/SoapyNanoZC.cpp`](src/soapy/SoapyNanoZC.cpp),
  `driver=nanozc`) with a free-running ring buffer, so a GUI that hitches for a few ms never starves
  or back-pressures the radio.

---

## Using it

### With SoapySDR apps (SCEPTRE, CubicSDR, SDRangel, GQRX-soapy, …)
Build and install the host module (see [Host driver](#host-driver-soapysdr-module)). The device then
shows up as:

```
$ SoapySDRUtil --find="driver=nanozc"
  driver = nanozc
  label  = PlutoSDR Nano zero-copy
```

Select it like any Soapy device and set the sample rate up to 20 MS/s.

**SCEPTRE** needs extra care — it bundles its own SoapySDR and loads plugins from its *own*
modules dir, and the device must be selected as **"PlutoSDR Nano zero-copy"** (not the built-in
"Pluto SDR"). Full steps: **[`docs/SCEPTRE.md`](docs/SCEPTRE.md)**.

### With SDR++
SDR++'s built-in **PlutoSDR** source uses libiio — that is the slow path and will be choppy at high
rates. The zero-copy path goes through SDR++'s **SoapySDR** source module, which the official
builds do **not** ship. Install the `soapy_source.dylib` from the [Releases](../../releases) (macOS
arm64 / SDR++ v1.3.0) plus `libSoapyNanoZC.so` — see [`dist/README.md`](dist/README.md) — or build
the plugin for your SDR++ version with [`docs/SDRPP_MACOS.md`](docs/SDRPP_MACOS.md). Then: **Source**
dropdown → **SoapySDR** → **PlutoSDR Nano zero-copy** → 20 MS/s. (The stock Pluto source still works
at low rates unchanged.)

---

## Flashing

The image is a standard FIT written to the firmware partition over DFU — the same mechanism as an
official Pluto firmware update. **The first 1 MB (FSBL + U-Boot) is never written.**

```sh
# 1. put the board in DFU-SF mode (at the U-Boot "Pluto>" prompt):
Pluto> run dfu_sf

# 2. write Zero_SDR to the firmware partition (alt "firmware.dfu" = SPI 0x200000):
dfu-util -d 0456:b674 -a firmware.dfu -D firmware/zero_sdr_slim.itb
dfu-util -d 0456:b674 -a firmware.dfu -e      # detach; U-Boot auto-sets fit_size + saves env

# 3. power-cycle. It boots the appliance and auto-starts the interfaces.
```

The image is < 16 MB on purpose, so the whole firmware region stays below the QSPI 16 MB 4-byte
("EAR") boundary and reads back with plain 3-byte addressing.

### Returning to stock
The **original vendor firmware** read back from a stock unit is in
[`firmware/stock/stock_firmware_nano.dfu`](firmware/stock/) (md5 in
[`firmware/MD5SUMS`](firmware/MD5SUMS)). Flash it exactly as above, substituting that file. DFU is
reachable from U-Boot even if Linux will not boot, so you cannot lock yourself out this way. Full
recovery ladder (U-Boot, SPI, backstops) is in [`docs/RESTORE.md`](docs/RESTORE.md).

---

## Host driver (SoapySDR module)

```sh
# needs SoapySDR dev headers + libusb-1.0
c++ -std=c++17 -shared -fPIC -O2 src/soapy/SoapyNanoZC.cpp \
    $(pkg-config --cflags --libs SoapySDR libusb-1.0) \
    -o libSoapyNanoZC.so
# install into your SoapySDR modules dir, e.g.:
cp libSoapyNanoZC.so "$(SoapySDRUtil --info | awk -F': ' '/Search path/{print $2}')/"
```

Test/benchmark tools are in [`src/soapy/`](src/soapy/): `jitterprobe` (real-time smoothness),
`zctx`/`zcloop` (TX + on-air self-reception), `zctest`.

---

## Building the firmware image

See [`build/BUILD.md`](build/BUILD.md). In short: the ADI 6.12 kernel + Tezuka patches + a few
upstream DMABUF leak backports, a device tree with read-only QSPI partitions and
`dr_mode = "peripheral"` ([`build/fdt_slim.dts`](build/fdt_slim.dts)), a slimmed rootfs that
autostarts `iiod` + `zcd`, packed into a FIT ([`build/tezuka_slim.its`](build/tezuka_slim.its)).

---

## Transmit / legal

This image ships with the TX safety interlocks **disabled** (no frequency allow-list, no power
ceiling, no watchdog) because it was built for authorized work on an RF range. **You are solely
responsible** for operating within your license and local regulations: transmit only into a dummy
load or cabled/attenuated setup unless you are licensed and authorized for the frequency and power.
Known issue: 8-bit (CS8) TX has a double-sideband/image artifact; use 16-bit TX for a clean signal.

## Credits & license
Built on [Analog Devices plutosdr-fw](https://github.com/analogdevicesinc/plutosdr-fw) and
[F5OEO Tezuka](https://github.com/F5OEO/plutosdr-fw). Kernel and firmware components are GPL-2.0;
their licenses apply to the corresponding parts here. The zero-copy daemon, SoapySDR module, and
tooling in this repo are provided as-is for research and interoperability.
