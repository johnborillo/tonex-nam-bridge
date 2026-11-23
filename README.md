# tonex-nam-bridge

Recapture NAM models (e.g. from TONE3000) as ToneX tone models, with no DAW, virtual cables or
audio interface involved.

**NAM Bridge** is a virtual audio device: a CoreAudio driver on macOS and an ASIO driver on
Windows ([Windows instructions](#windows-the-nam-bridge-asio-driver)). To ToneX Modeler it looks like an ordinary
audio interface with a reamp box and an amp behind it: whatever ToneX plays to the
*NAM Bridge* output comes back on the *NAM Bridge* input, processed by the `.nam` model you
loaded. ToneX captures it exactly as it would capture a real amp.

```
ToneX Modeler ──out 1──▶ [ NAM Bridge device: NAM model ] ──in 1──▶ ToneX Modeler
```

- The audio is bit-identical to NAM's own reference renderer (exact `tanh`, no approximations).
- Never leaves the computer: no noise, no converters, no clock drift.
- 44.1 / 48 / 88.2 / 96 kHz. If ToneX runs at a rate other than the model's (usually 48 kHz), the
  bridge resamples around the model.
- Supports WaveNet (incl. A2), LSTM and the other architectures NAM core supports.

## Build (macOS)

```bash
scripts/bootstrap.sh                       # fetch NAM core + libASPL at tested commits
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
scripts/build-app.sh                       # menu-bar app -> build/NAM Bridge.app
```

## Option A: prototype with BlackHole (no install)

Uses BlackHole 16ch as the cable: ToneX plays on ch1, the bridge returns the amp on ch3.

```bash
build/nam-bridge bridge --model "path/to/capture.nam" --device "BlackHole 16ch" --in 1 --out 3
```

In ToneX Modeler set the audio device to *BlackHole 16ch*, output **1**, input **3**.
The terminal needs Microphone permission to read BlackHole's input.

## Option B: the NAM Bridge driver

```bash
scripts/install-driver.sh      # sudo; restarts coreaudiod (audio drops for a moment)
open "build/NAM Bridge.app"    # menu-bar icon: Load .nam…, levels, meters
```

In ToneX Modeler choose **NAM Bridge** as the audio device (output 1, input 1) and run a normal
capture. To switch captures, load another `.nam` from the menu-bar app. The driver remembers the
last model and levels across restarts.

Everything the app does is also available from the CLI:

```bash
build/nam-bridge driver load "capture.nam"
build/nam-bridge driver set --in-gain 0 --out-gain -3 --input left
build/nam-bridge driver watch            # live status + meters
build/nam-bridge driver unload
scripts/uninstall-driver.sh
```

## Levels

The bridge is all digital, so gain staging is just numbers:

- **Input gain** is applied to ToneX's test signal before the model. Leave it at 0 dB unless
  the capture's creator documented a different reamp level. ToneX's level is passed to the model
  sample for sample, exactly as a reamp box at unity would.
- **Output gain** is applied to the model's output before ToneX records it. Lower it if the Out
  meter goes red (clipping).
- **Source channel**: which NAM Bridge output channel feeds the model. Use Left if ToneX plays on
  output 1. If the In meter stays at -inf during a capture, switch to Right or L+R.

## Checks

```bash
build/nam-bridge selftest "capture.nam"                 # engine + loopback, no devices
build/driver_harness build/NAMBridge.driver "capture.nam" # loads the driver in-process, like coreaudiod
build/nam-bridge loopcheck --model "capture.nam" --device "NAM Bridge" --play 1 --rec 1
build/nam-bridge render "capture.nam" di.wav out.wav    # offline render, same engine
build/nam-bridge compare out.wav tests/vectors/expected-clubman-ssr-od-lo-48k.wav
```

`loopcheck` plays a test signal through a device, records what comes back, aligns the two, and
compares them with an offline NAM render. That is what ToneX does, so it is the end-to-end proof.

## Layout

| Path | What it is |
| --- | --- |
| `engine/NamEngine.*` | NAM model holder: realtime-safe hot swap, gains, resampling |
| `engine/Loopback.h` | Sample-time-stamped ring: output at time T is readable as input at T |
| `engine/DriverProtocol.h` | Custom CoreAudio property IDs shared by driver, CLI and app |
| `driver/Driver.cpp` | AudioServerPlugIn (libASPL) exposing the "NAM Bridge" device |
| `cli/main.cpp` | `nam-bridge` CLI: prototype bridge, probe, render, driver control, tests |
| `app/Sources/*.swift` | Menu-bar app |
| `tests/driver_harness.cpp` | Fake HAL host that drives the built plug-in |
| `asio/AsioDriver.*` | Windows ASIO driver (IASIO): clock thread, loopback, buffers |
| `asio/ComServer.cpp` | COM class factory and (un)registration |
| `asio/ControlPanel.cpp` | Win32 settings dialog |
| `asio/Settings.*` | Per-user settings in HKCU |
| `tests/asio_harness.cpp` | Fake ASIO host that drives the built driver |
| `scripts/*-asio.ps1` | Register / unregister the ASIO driver |

## Windows: the NAM Bridge ASIO driver

On Windows, NAM Bridge is a virtual **ASIO driver**: a DLL that ToneX Modeler loads like any
audio interface's driver. There's no kernel driver, virtual cable or audio hardware involved.
Whatever ToneX plays to *NAM Out 1* comes back on *NAM In 1*, processed by the `.nam` model,
exactly one buffer later. The driver is its own clock.

```
ToneX Modeler ──ASIO out 1──▶ [ NAMBridgeASIO.dll: NAM model ] ──ASIO in 1──▶ ToneX Modeler
```

Tested with TONEX Modeler 2.3.1 on Windows 11. The output matches NAM's own reference
renderer (−131 dB difference, i.e. identical within float rounding).

### 1. Install the build tools (once)

- **Visual Studio 2022 Build Tools** with the *Desktop development with C++* workload:
  ```powershell
  winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  ```
- **CMake** ≥ 3.24 and **Git**: `winget install --id Kitware.CMake -e`, `winget install --id Git.Git -e`

### 2. Get the code and dependencies

```powershell
git clone https://github.com/johnborillo/tonex-nam-bridge.git
cd tonex-nam-bridge
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1   # NAM core at the tested commit
```

**Steinberg ASIO SDK.** Download it from [steinberg.net/developers](https://www.steinberg.net/developers/)
(2.3.4 or later). Accept its license, choosing the **GPLv3** option (see
[asio/COPYING.md](asio/COPYING.md)). Extract it so that this file exists:

```
deps\asiosdk\common\asio.h
```

The zip contains a versioned folder (e.g. `ASIO-SDK_2.3.4_2025-10-15`), and depending on the
release a nested `ASIOSDK` folder inside it. The folder that directly contains `common\`,
`host\` and `driver\` must be named `deps\asiosdk`.

### 3. Build and test

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

This builds `build\Release\nam-bridge.exe` (CLI), `build\asio\Release\NAMBridgeASIO.dll` (the
driver) and `build\asio\Release\asio_harness.exe` (a fake ASIO host that tests the driver).

```powershell
build\Release\nam-bridge.exe selftest "path\to\model.nam"                 # engine checks
build\asio\Release\asio_harness.exe --dll build\asio\Release\NAMBridgeASIO.dll --model "path\to\model.nam"
```

`asio_harness` loads the driver like a host. It checks every ASIO call, plays a DI file through
the loopback at 48 and 44.1 kHz, compares the result with an offline render (bit-exact), measures
timing, and hot-swaps models while running. It ends with `All checks passed`. It temporarily uses
the driver's settings and restores yours afterwards.

### 4. Install the driver

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install-asio.ps1        # UAC prompt
build\asio\Release\asio_harness.exe --model "path\to\model.nam"          # same tests, via the registry, as ToneX loads it
```

`install-asio.ps1` registers the DLL **where it is** (`build\asio\Release\`). Don't delete or move
the `build` folder afterwards. If you do move it, run `install-asio.ps1` again from the new location.

### 5. Use it in ToneX Modeler

1. In ToneX Modeler's audio settings choose driver type **ASIO** and device **NAM Bridge**.
   Use **48 kHz** if you can choose (most NAM models are 48 kHz; at 44.1 kHz the bridge resamples).
2. Click the driver's settings / control panel button. The **NAM Bridge** window opens:
   - **Load .nam…** picks the capture to recapture; **Unload** removes it (input goes silent).
   - **Input gain / Output gain**: −24…+24 dB (see [Levels](#levels)).
   - **Source channel**: which ASIO output feeds the model (Left = out 1, Right = out 2, L+R).
   - **Meters**: in/out peak level; status line with sample rate, buffer size and DSP load.
   - Every change applies immediately and is remembered (stored in
     `HKCU\Software\tonex-nam-bridge\NAMBridgeASIO`).
3. Run a normal capture with **output 1** and **input 1**. During the test signal the dialog's
   **In** meter moves. If it stays at −inf, ToneX is playing on output 2: set Source channel to Right.
4. To recapture another model, load the next `.nam` in the dialog and run a new capture.

### Updating

- **ToneX Modeler updates** don't affect the driver. It's installed separately and stays
  registered. After an update, check that ToneX still shows **NAM Bridge** under ASIO. If it
  doesn't, run `scripts\install-asio.ps1` again.
- **Updating this project** (`git pull`): close ToneX Modeler (it keeps the DLL loaded, which
  blocks the build), then rebuild with `cmake --build build --config Release`. No re-install is
  needed as long as the `build` folder didn't move. Re-run `asio_harness` to check.
- **Updating NAM core** (`scripts\bootstrap.ps1` pins a tested commit): after changing the pin,
  rebuild and run `selftest` / `compare` / `asio_harness` before using it for captures.

### Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File scripts\uninstall-asio.ps1      # UAC prompt
reg delete "HKCU\Software\tonex-nam-bridge" /f                           # optional: forget settings
```

### Troubleshooting (Windows)

| Symptom | Fix |
|---|---|
| ToneX doesn't list NAM Bridge | Run `scripts\install-asio.ps1` again; check `HKLM\SOFTWARE\ASIO\NAM Bridge` exists. |
| In meter stays at −inf during a capture | ToneX plays on output 2: set Source channel to Right (or L+R). |
| Out meter hits 0 dBFS | Lower Output gain. |
| Dropouts / crackles, high "DSP load" in the dialog | Use a bigger buffer in ToneX, or the Windows *High performance* power plan. CPU power saving can slow real-time processing several times over. |
| Dialog shows "Error: Cannot open …" | The stored model file moved: load it again. |

The full design and test spec is in [docs/WINDOWS_ASIO.md](docs/WINDOWS_ASIO.md).

**Related:** [tonex-amd-gpu](https://github.com/johnborillo/tonex-amd-gpu) makes ToneX Modeler
*train* captures on an AMD Radeon GPU (by default it uses the GPU only on NVIDIA cards).

## Notes

- The driver bundle is ad-hoc signed for local use. Sharing it with other Macs would need a
  Developer ID signature and notarization.
- Captures on TONE3000 carry their creators' licenses. Recapturing for your own use is one thing;
  posting the ToneX versions (e.g. on ToneNET) may not be allowed for a given capture.

## License

GNU General Public License v3.0: see [LICENSE](LICENSE). You may use, modify and redistribute
this project. If you distribute it, or anything built from it, you must do so under GPLv3 and
make the source available.

The Windows ASIO driver has to be GPLv3, because it's built with Steinberg's ASIO SDK under the
SDK's GPLv3 option (details in [asio/COPYING.md](asio/COPYING.md)). Third-party code fetched into
`deps/` keeps its own license: NeuralAmpModelerCore and libASPL are MIT.

ToneX and TONEX are trademarks of IK Multimedia, ASIO of Steinberg Media Technologies, and
NAM / Neural Amp Modeler of its respective owner. This project is not affiliated with any of them.
