# Prebuilt plugins & drivers

Binaries are attached to the [GitHub Releases](../../releases) page, not committed here.
This folder documents what they are and how to install them.

## Platform

The host driver `libSoapyNanoZC.so` is prebuilt for three targets:

| Folder | Target | Needs |
|---|---|---|
| `macos-arm64/` | macOS Apple Silicon | `brew install soapysdr libusb` (SoapySDR 0.9) |
| `linux-x86_64/` | Linux x86-64 | `libsoapysdr0.8` + `libusb-1.0-0` (SoapySDR 0.8 ABI) |
| `linux-aarch64/` | Linux ARM64 | `libsoapysdr0.8` + `libusb-1.0-0` (SoapySDR 0.8 ABI) |

The Linux builds link the standard `libSoapySDR.so.0.8` / `libusb-1.0.so.0` SONAMEs, so any current
Ubuntu/Debian with `libsoapysdr0.8` works (`sudo apt install libsoapysdr0.8 libusb-1.0-0`). If your
distro ships a different SoapySDR ABI, build from source — one command, see
[`../src/soapy/`](../src/soapy/). The SDR++ plugin (`soapy_source.dylib`) is **macOS arm64 /
SDR++ v1.3.0** only; other versions/platforms build it per [`docs/SDRPP_MACOS.md`](../docs/SDRPP_MACOS.md).

macOS downloads are quarantined; clear it: `xattr -dr com.apple.quarantine <file>`.

## `libSoapyNanoZC.so` — the host driver (needed by BOTH SDR++ and SCEPTRE)
The SoapySDR module that talks to the board's zero-copy interface. Install into your SoapySDR
modules directory:

```sh
cp libSoapyNanoZC.so "$(SoapySDRUtil --info | awk -F': ' '/Search path/{print $2}')/"
SoapySDRUtil --find="driver=nanozc"   # should list "PlutoSDR Nano zero-copy"
```

- **SCEPTRE**: that's all — it uses SoapySDR natively, so `nanozc` now appears.
- **SDR++**: also needs the plugin below (stock SDR++ ships no SoapySDR source).

## `soapy_source.dylib` — the SDR++ SoapySDR source plugin
Lets SDR++ use SoapySDR at all. **Only matches SDR++ v1.3.0 macOS arm64** (nightly built from
commit `8c9f5ee8`); other builds need it compiled against their own core
(see [`docs/SDRPP_MACOS.md`](../docs/SDRPP_MACOS.md)).

```sh
cp soapy_source.dylib /Applications/SDR++.app/Contents/Plugins/     # or wherever SDR++.app is
codesign -f -s - /Applications/SDR++.app/Contents/Plugins/soapy_source.dylib
codesign -f -s - --deep /Applications/SDR++.app                     # re-seal the bundle
```

Then add an enabled module instance to `~/Library/Application Support/sdrpp/config.json` under
`moduleInstances`:

```json
"SoapySDR": { "enabled": true, "module": "soapy_source" }
```

Launch SDR++ → **Source** dropdown → **SoapySDR** → pick **PlutoSDR Nano zero-copy** → set 20 MS/s.

## `zero_sdr_slim.itb` — the firmware
The board must be running Zero_SDR firmware (it provides the `zcd` zero-copy daemon). Flash per the
[top-level README](../README.md#flashing). The stock firmware for restoring is at
[`../firmware/stock/`](../firmware/stock/).
