# Building the SDR++ `soapy_source` plugin (macOS)

Stock SDR++ release builds do **not** ship a SoapySDR source module, so SDR++ can't use the
`nanozc` zero-copy driver out of the box. This builds just that one plugin, ABI-matched to your
SDR++ build, and drops it into the existing app — no need to rebuild all of SDR++.

## Why it must match your build
An SDR++ plugin is a `.dylib` that dlopens into the app's `libsdrpp_core.dylib`. It only loads if
it was compiled from the **same source commit** as your app's core. Find your build's version:

```sh
strings /Applications/SDR++.app/Contents/Frameworks/libsdrpp_core.dylib | grep "SDR++ v"
# e.g. "SDR++ v1.3.0 (Built at 00:39, Jul 5 2026)"  -> use the commit from just before that date
```

## Build
```sh
brew install cmake ninja glfw volk spdlog fftw zstd soapysdr libusb fmt
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git && cd SDRPlusPlus
git checkout 8c9f5ee8        # the commit matching the 1.3.0 nightly above; pick yours by date

# configure with everything off except the Soapy source, then build only core + the module
OFFS=$(grep -oE 'OPT_BUILD_[A-Z_]+' CMakeLists.txt | sort -u | sed 's/^/-D/;s/$/=OFF/')
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release ${OFFS} -DOPT_BUILD_SOAPY_SOURCE=ON   # zsh: use ${=OFFS}
cmake --build build --target sdrpp_core soapy_source -j8
```

## Install into your app
```sh
APP=/Applications/SDR++.app ; MOD=build/source_modules/soapy_source/soapy_source.dylib
cp "$MOD" "$APP/Contents/Plugins/soapy_source.dylib" ; DST="$APP/Contents/Plugins/soapy_source.dylib"

# point its deps at the app's own bundled libs (avoid loading a 2nd copy of glfw/volk/fftw/zstd)
for l in libglfw.3 libfftw3f.3 libvolk.3.3 libzstd.1; do
  cur=$(otool -L "$DST" | awk "/opt\/homebrew.*$l\.dylib/{print \$1; exit}")
  [ -n "$cur" ] && install_name_tool -change "$cur" "@rpath/$l.dylib" "$DST"
done
# keep only the app Frameworks on the search path; drop Homebrew rpaths so @rpath resolves to bundled libs
install_name_tool -add_rpath "@loader_path/../Frameworks" "$DST" 2>/dev/null
for rp in $(otool -l "$DST" | awk '/LC_RPATH/{f=1} f&&/path \/opt\/homebrew/{print $2; f=0}'); do
  install_name_tool -delete_rpath "$rp" "$DST"; done

codesign -f -s - "$DST" ; codesign -f -s - --deep "$APP" ; xattr -dr com.apple.quarantine "$APP"
```

Register it in `~/Library/Application Support/sdrpp/config.json` under `moduleInstances`:
`"SoapySDR": { "enabled": true, "module": "soapy_source" }`.

## Verify before launching (optional)
Every core symbol the module needs must exist in your app's core, and it should dlopen cleanly:

```sh
# no output = all core symbols present (ignore std::/Soapy/volk/OpenGL — those come from their own libs)
comm -23 <(nm -u "$DST"|awk '{print $1}'|sort -u) \
         <(nm -gU "$APP/Contents/Frameworks/libsdrpp_core.dylib"|awk '{print $NF}'|sort -u) \
 | c++filt | grep -iE 'dsp|sigpath|gui|flog|SmGui|Config|ModuleManager|SourceManager'
```
