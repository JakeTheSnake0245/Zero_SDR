# Using Zero_SDR with SCEPTRE

SCEPTRE talks to SDRs through **SoapySDR**, so the zero-copy driver works with it — but SCEPTRE is
self-contained and has two quirks that matter:

1. **It bundles its own SoapySDR** and loads plugins from its **own** modules directory, not the
   system one. Installing the driver in `/usr/lib/.../SoapySDR/modules0.8` does **nothing** for
   SCEPTRE (and can disturb it). The driver must go in SCEPTRE's own dir.
2. **It opens devices through a `SoapySDRServer`** (SoapyRemote). That server loads the driver and
   sets sample rate, frequency, and gain on open — so the driver must accept those cleanly.

The prebuilt Linux driver in the [release](../../releases) already handles the open sequence
(it clamps out-of-range frequencies and forces manual gain mode when a gain is set). Build from
source only if your SoapySDR ABI differs — see [`../src/soapy/`](../src/soapy/).

## Install (Linux, SCEPTRE 4.x)

Find SCEPTRE's install dir and its modules dir (the version string varies):

```sh
SCEPTRE=$(ls -d /opt/sceptre-*-ubuntu-* | head -1)      # e.g. /opt/sceptre-4.18.1-ubuntu-22.04-...
MDIR="$SCEPTRE/lib/SoapySDR/modules0.8"
ls "$MDIR"                                              # you should see libPlutoSDRSupport.so etc.
```

Build on the SCEPTRE host (matches its SoapySDR 0.8 ABI) or use the release binary, then copy in:

```sh
# option A: use the release binary (Linux x86-64)
curl -L -o libSoapyNanoZC.so \
  https://github.com/JakeTheSnake0245/Zero_SDR/releases/latest/download/libSoapyNanoZC-linux-x86_64.so
# option B: build it (needs libsoapysdr-dev libusb-1.0-0-dev g++)
#   c++ -std=c++17 -shared -fPIC -O2 src/soapy/SoapyNanoZC.cpp \
#       $(pkg-config --cflags --libs SoapySDR libusb-1.0) -o libSoapyNanoZC.so

sudo cp libSoapyNanoZC.so "$MDIR/libSoapyNanoZC.so"
```

That's it — it's purely additive; every existing SCEPTRE driver stays. (On macOS the driver goes in
the Homebrew modules dir instead; SCEPTRE there uses the system SoapySDR.)

## Select it in SCEPTRE

1. Plug the Nano (running Zero_SDR firmware) into the SCEPTRE machine. Give it a few seconds to
   enumerate — right after a physical replug there's a brief window where a connect reports
   "interface not found"; just wait and retry.
2. Fully **quit and reopen SCEPTRE** after installing/updating the driver, so it launches a fresh
   server that loads the new plugin.
3. In the SDR source list pick **"PlutoSDR Nano zero-copy"** — the entry whose SoapySDR driver is
   `nanozc`.
   - **Do NOT pick "Pluto SDR"** — that is SCEPTRE's built-in libiio driver (the slow path), and it
     has a separate bug that crashes on open with this board. The zero-copy entry is the one you
     want anyway (up to 20 MS/s, no drops).
4. Set your center frequency and sample rate (up to 20 MS/s) and open it.

## Verify from the shell (optional)

Using SCEPTRE's own SoapySDR, confirm the device probes cleanly:

```sh
SCEPTRE=$(ls -d /opt/sceptre-*-ubuntu-* | head -1)
LD_LIBRARY_PATH="$SCEPTRE/lib:$SCEPTRE/deps/lib" \
SOAPY_SDR_PLUGIN_PATH="$SCEPTRE/lib/SoapySDR/modules0.8" \
SoapySDRUtil --probe="driver=nanozc"
```

You should see device identification, `Channels: 1 Rx, 1 Tx`, and the sensor list.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `board rejected parameter 1: Invalid argument` | Old driver without the frequency clamp — update to the current release build. |
| `board rejected parameter 7: Operation not supported` | Old driver without the gain-mode fix — update to the current release build. |
| `zero-copy interface not found (is zcd running?)` | Board mid-reenumeration after a replug (wait a few seconds), or `zcd` died — power-cycle the board to restart it. |
| Device not in the list | Driver not in SCEPTRE's **own** `modules0.8` dir, or SCEPTRE wasn't restarted after install. |
