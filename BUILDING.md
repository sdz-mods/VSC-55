# Building VSC-55 A00

Build on modern Windows using PowerShell, Git, CMake 3.23+, Ninja and Python 3.
The target binaries run on Windows 98 SE; the build tools do not need to.
Do not substitute a current generic MinGW/UCRT build for the Win98 toolchain.

## 1. Source and pinned core

```powershell
git clone --recurse-submodules https://github.com/sdz-mods/VSC-55.git
cd VSC-55
```

For an existing checkout, run `git submodule update --init --recursive`.
Nuked-SC55 is pinned to release **0.7.0**, commit
`02f6e3d7bad89af33514bd48211bb950f8ad0e6b`. Do not update it implicitly.
Source ZIPs produced by the packaging script include the checked-out core.

## 2. Win98 GCC toolchain

```powershell
powershell -ExecutionPolicy Bypass -File tools/get-mingw-lite.ps1
```

This downloads MinGW Lite `16.2.0+emutls-r1`, profile
`mingw32_686-msvcrt_win98`, and verifies archive SHA-256
`d51c5103f9cb4f92cfaae6fb0045512683097307f31dbd759457784d9761f4e3`.
The script uses curl and Windows tar with 7z support. If extraction is unsupported,
extract the verified archive using 7-Zip into `tools/mingw-lite`.
Expected compiler: `tools/mingw-lite/mingw32_686-msvcrt_win98-16+emutls/bin/g++.exe`.

## 3. Open Watcom for the Win16 driver

Install [Open Watcom v2](https://github.com/open-watcom/open-watcom-v2) into
`tools/ow`, or pass `-WatcomRoot` below. The tested compiler identifies as
**2.0 beta, Jun 23 2026, 64-bit host**. Required: `binnt64/wcc.exe`,
`binnt64/wlink.exe`, `h/win`, `h`, `lib286/win` and `lib286`.
Toolchains are not committed or included in source archives.

## 4. Compile

```powershell
powershell -ExecutionPolicy Bypass -File tools/build-driver.ps1
powershell -ExecutionPolicy Bypass -File tools/build-panel.ps1
```

Optional installed-toolchain overrides:

```powershell
./tools/build-driver.ps1 -WatcomRoot D:/Toolchains/Watcom
./tools/build-panel.ps1 -CompilerRoot D:/Toolchains/MinGW/bin
```

Outputs: `build/driver16/VSC55.DRV`, and `build/panel/{vsc55,vsccfg,midireg}.exe`.
The production default is GCC **-O3 + LTO, -march=i686 -mtune=generic**, MSVCRT,
static compiler runtime, original decoder, oversampling retained. No fast-math.
The cached decoder remains disabled because PCM comparison tests found output differences.
`-TimingDiagnostics` produces a separate `build/panel-timing` build.

## 5. Package

```powershell
python tools/package-release.py
```

Creates `dist/VSC55-A00/` and `dist/VSC55-A00.zip`, including the installer,
notices, and matching `SOURCE.ZIP`. It excludes ROMs, toolchains, build artifacts,
local logs and Git metadata from source. Review the package before publishing.
Each run recreates `dist/VSC55-A00/`, removing any stale staging files.
The script requires the compiled files and initialized core sources.

## Verification

```powershell
cmake --build build/panel --target dcoutput_test samplefifo_test mute_test coreprobe miditest miditone
./build/panel/dcoutput_test.exe
./build/panel/samplefifo_test.exe mk1-v1.21 C:/PrivateROMs/MK1/1.21
./build/panel/mute_test.exe mk2-v1.01 C:/PrivateROMs/MK2/1.01
python tests/verify-package.py
```

To exercise all output sample rates, fallback values and captured WAV durations:

```powershell
python tests/sample-rates.py build/panel/vsc55.exe C:/PrivateROMs --baseline C:/PreviousBuild/vsc55.exe
```

This test expects `SC-55-v1.21` and `SC-55mk2-v1.01` below the ROM root.
The optional baseline checks that 48 kHz PCM remains byte-identical. Captures
and logs stay in ignored `build/sample-rate-results`; no sound device is opened.

The ROM-dependent tests require your own dumps. Driver registration and end-to-end
MIDI tests must run on Windows 98; modern-host tests cannot establish Win98
shutdown, thunking, or sound-driver behavior. Never register the driver on modern Windows.

## Source formatting

Project-owned C/C++ uses AStyle 3.1 with the checked-in `.astylerc`:
four-space indentation, Allman braces and expanded single-line statements.
From the repository directory in WSL, run:

```sh
git ls-files -z -- '*.c' '*.cpp' '*.h' | xargs -0 astyle --options=.astylerc
```

The tracked-file list excludes the contents of the upstream submodule. Leave
upstream sources unchanged. Formatting preserves Windows CRLF line endings.

## Icon and revision

The icon is editable 16x16 pixel geometry in `assets/vsc55-icon.svg`.
Regenerate with `python tools/package-icon.py` (requires Pillow).
The application revision is defined in `src/common/version.h`.
