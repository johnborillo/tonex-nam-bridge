# NAM Bridge for Windows: virtual ASIO driver (implementation spec)

This spec is for a Claude Code session running **on the Windows PC**. It is self-contained:
the session that wrote it ran on macOS and has no access to Windows, so the Windows build,
tests and ToneX check all happen there.

**Prompt to start the Windows session:**
> Read `docs/WINDOWS_ASIO.md` and implement it. Follow the milestones in order and don't
> move to the next one until the current one's checks pass.

---

## 1. Goal

ToneX Modeler builds ToneX tone models by playing a test signal into a real amp and recording
the result. NAM Bridge makes a NAM capture (`.nam`, e.g. from TONE3000) act as that amp, so
NAM captures can be recaptured as ToneX models with no DAW, cables or audio interface.

On macOS this already works as a CoreAudio virtual device: a capture recorded through it
trained into a ToneX model that the owner says sounds very close to the NAM original.
On Windows we build the same thing as a **virtual ASIO driver**:

```
ToneX Modeler ──ASIO out ch1──▶ [ NAMBridgeASIO.dll: NAM model ] ──ASIO in ch1──▶ ToneX Modeler
```

An ASIO driver is an in-process COM DLL that the host (ToneX Modeler) loads into itself.
**Do not write a kernel driver.** No driver signing, no virtual cables, no routing.

## 2. What already exists (portable, verified on macOS)

| Path | Status |
| --- | --- |
| `engine/NamEngine.{h,cpp}` | NAM model holder. Realtime-safe hot swap, input/output gain, Lanczos resampling when host rate ≠ model rate, peak meters. Portable C++20. **Reuse as is.** |
| `engine/Loopback.h` | macOS ring buffer. Only its `InputSelect` enum is useful here. |
| `cli/main.cpp` | `nam-bridge` CLI. On Windows only `render`, `selftest` and `compare` are built (the rest is CoreAudio, behind `NB_COREAUDIO`). |
| `CMakeLists.txt` | Builds `namcore`, `nambridge_engine`, `nam-bridge` on any OS. On `WIN32` it already does `add_subdirectory(asio)` if `asio/CMakeLists.txt` exists. MSVC: static CRT, `/fp:fast`, `_USE_MATH_DEFINES`, `NOMINMAX`. |
| `driver/`, `app/`, `tests/driver_harness.cpp` | macOS only. Read `driver/Driver.cpp` to see how the Mac driver uses the engine. |
| `tests/vectors/` | `di-48k.wav` plus `expected-clubman-ssr-od-lo-48k.wav`: NAM's own reference render of that DI through the model in §9. |

`NamEngine` API, as used by the driver:

```cpp
nambridge::NamEngine engine;
engine.SetHostRate(48000.0);                         // non-RT; rebuilds model if rate changed (~100 ms)
engine.LoadFromJson(jsonText, displayName, error);   // non-RT; atomic swap; old model kept on failure
engine.Unload();                                     // non-RT
engine.SetInputGainDb(db); engine.SetOutputGainDb(db); // any thread
engine.Info();                                       // non-RT: name, architecture, modelRate, hostRate, resampling...
engine.TakePeaks(inPeak, outPeak);                   // any thread; linear, resets
engine.ResetState();                                 // any thread; RT thread re-primes the model
engine.Process(in, out, n);                          // RT, exactly one thread; any n; silence if no model
```

Facts from the Mac work that also apply here:
- **NAM's static registration must survive linking.** NAM registers architectures from static
  initializers. The CMake already links `namcore` with `WHOLE_ARCHIVE` (`/WHOLEARCHIVE` on
  MSVC). If you ever see `No config parser registered for architecture: WaveNet`, this is broken.
- **Exact `tanh`.** The engine disables NAM's `fast_tanh` on purpose; capture fidelity beats
  the few percent of CPU it saves. On macOS the output is bit-identical to NAM's renderer.
- **Resampling.** At the model's rate (48 kHz for almost all NAM files) the path is exact. At
  44.1 kHz the engine resamples. Against an offline render the error is about −41 dB, rising
  smoothly with frequency. That's a sub-sample phase difference between two valid
  resamplings, not glitches. Prefer 48 kHz.
- **CPU.** One standard WaveNet model runs about 10× faster than real time on an i5-11400F.
- **Don't change the sample rate on the host.** On macOS, ToneX Modeler crashed (null
  function pointer in its device-change listener) when the device's sample rate changed under
  it. In ASIO only the host sets the rate; the driver and its control panel must never change
  it.

## 3. Prerequisites on the PC

1. Visual Studio 2022 (or Build Tools 2022) with **Desktop development with C++**, CMake ≥ 3.24, Git.
2. Copy the project over (without `build/` and `deps/`), then from the repo root run
   `powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1`.
   This fetches NeuralAmpModelerCore at the pinned commit (with Eigen and AudioDSPTools submodules).
3. **ASIO SDK:** download Steinberg's ASIO SDK (2.3.4 or later, which offers GPLv3 alongside
   the proprietary license) from Steinberg's developer site (steinberg.net/developers).
   Extract it so that `deps\asiosdk\common\asio.h` and `deps\asiosdk\common\iasiodrv.h` exist.
   Read its license file and confirm the GPLv3 option before using it.
   **Licensing consequence:** the `asio/` component is GPLv3 (the owner agreed). Add
   `asio/COPYING.md` saying so. NAM core is MIT, which is compatible.
4. The test model ships in `testdata/` inside the project zip (git-ignored). See §9.

## 4. Milestone 1: engine and CLI build on Windows

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\nam-bridge.exe selftest "<model.nam>"
build\Release\nam-bridge.exe render "<model.nam>" tests\vectors\di-48k.wav build\out48.wav
build\Release\nam-bridge.exe compare build\out48.wav tests\vectors\expected-clubman-ssr-od-lo-48k.wav
```

Pass criteria:
- `selftest` prints `All checks passed`.
- `compare` passes at its default −80 dB threshold. MSVC with `/fp:fast` may not be
  bit-identical to clang/NAM; anything ≤ −80 dB is fine. If it fails, try `/fp:precise` on
  `namcore` and the engine before anything else, and report the numbers.

Expect small portability fixes (MSVC warnings or errors in the engine or CLI). Fix them
without changing behavior. Keep the macOS build working: don't remove `NB_COREAUDIO` guards.

## 5. Milestone 2: the ASIO driver (`asio/`)

### Files

```
asio/CMakeLists.txt        SHARED library NAMBridgeASIO (x64), links nambridge_engine
asio/AsioDriver.{h,cpp}    class implementing IASIO
asio/ComServer.cpp         class factory, DllGetClassObject/CanUnloadNow/Register/Unregister
asio/ControlPanel.cpp      Win32 settings dialog (controlPanel())
asio/Settings.{h,cpp}      HKCU persistence
asio/exports.def           exports only the four Dll* functions (PRIVATE)
asio/COPYING.md            GPLv3 note
tests/asio_harness.cpp     fake ASIO host (milestone 3)
scripts/install-asio.ps1   elevated regsvr32 of build\asio\Release\NAMBridgeASIO.dll
scripts/uninstall-asio.ps1 regsvr32 /u
```

Link: `nambridge_engine ole32 oleaut32 uuid advapi32 user32 comdlg32 comctl32 avrt winmm`.
Include `deps/asiosdk/common`. Use only the SDK headers (`asio.h`, `asiosys.h`, `iasiodrv.h`).
Write your own COM boilerplate instead of the SDK's sample `combase.cpp`; it's about 150
lines and avoids MFC-era baggage.

### Identity

- Driver name (ASIO list and `getDriverName`): **`NAM Bridge`** (≤ 31 chars)
- CLSID: **`{0FF6E346-784E-45F1-B522-7B6A486425EA}`**
- Registration (`DllRegisterServer`, run elevated):
  - `HKCR\CLSID\{0FF6E346-…}\InprocServer32` (default) = full DLL path, `ThreadingModel` = `Apartment`
  - `HKLM\SOFTWARE\ASIO\NAM Bridge`: `CLSID` = `{0FF6E346-…}` (string), `Description` = `NAM Bridge`
  - `DllUnregisterServer` removes both.
- **ASIO quirk:** hosts create drivers with `CoCreateInstance(clsid, …, CLSCTX_INPROC_SERVER, clsid, &p)`,
  passing the **CLSID as the IID**. `QueryInterface` must return the `IASIO*` when
  `riid == CLSID` (as well as for `IID_IUnknown`).

### Device model

| Property | Value |
| --- | --- |
| Channels | 2 in (`NAM In 1`, `NAM In 2`), 2 out (`NAM Out 1`, `NAM Out 2`) |
| Sample type | `ASIOSTFloat32LSB` for every channel |
| Sample rates | 44100, 48000, 88200, 96000 (`canSampleRate` → `ASE_NoClock` otherwise). Default 48000 |
| Buffer sizes | min 64, max 2048, preferred 256, granularity −1 (powers of two) |
| Clock sources | one: `Internal`, current |
| Latencies | input 0, output = buffer size (round trip is exactly one buffer, see below) |

### The loopback (core logic)

ASIO double-buffering: `bufferSwitch(i)` tells the host that input half `i` is valid and that
it must write output half `i`, which plays during the next period. Our driver has no
hardware. On each clock tick `k` (with `i = k & 1`):

1. **Take the output the host wrote on the previous tick:** half `(k-1) & 1` of the output
   channels. Pick the mono source by `InputSelect` (Left = out ch1, Right = out ch2,
   Mix = (L+R)/2).
2. `engine.Process(mono, processed, bufferSize)`.
3. **Write `processed` into input half `i`** of both input channels.
4. Call the host: `bufferSwitchTimeInfo(&asioTime, i, ASIOFalse)` if the host supports time
   info, otherwise `bufferSwitch(i, ASIOFalse)`.

So the host's output from tick `k−1` comes back as its input at tick `k`, a round trip of
exactly one buffer. On tick 0 the input is silence. Output half `(k-1)&1` is never the half
the host is writing at tick `k`, so there is no race.

### Clock thread (the driver is its own clock)

- Started by `start()`. `stop()` signals it and **joins it before returning**.
  `disposeBuffers()` stops first if running.
- Set `AvSetMmThreadCharacteristicsW(L"Pro Audio", …)` plus
  `AvSetMmThreadPriority(…, AVRT_PRIORITY_CRITICAL)`, and `timeBeginPeriod(1)` while running.
- Schedule on absolute `QueryPerformanceCounter` deadlines: `next = t0 + k * period`. Wait with
  a high-resolution waitable timer
  (`CreateWaitableTimerExW(…, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)`), then
  spin for the last ~200 µs. Never schedule relative to "now" (that drifts). If you wake more
  than 2 periods late, resynchronize (`t0` = now, keep `k`), count an overrun, and carry on.
- Keep `samplePosition = k * bufferSize` and the system time (ns, from QPC) of the current
  tick for `getSamplePosition` and `ASIOTime` (`kSystemTimeValid | kSamplePositionValid |
  kSampleRateValid`).
- **No allocations, locks, file or registry I/O, or logging in this thread.** All buffers are
  allocated in `createBuffers`.

### Other IASIO methods

- `init(sysHandle)`: load settings (§6). If a model path is stored, read the file and
  `LoadFromJson`; on failure keep going with no model and remember the error for
  `getErrorMessage`. `SetHostRate(current rate)`. Return `ASIOTrue`. Hosts may create and
  `init` the driver just to list it, so keep it robust.
- `setSampleRate(r)`: accept only supported rates. Store it, call `engine.SetHostRate(r)`
  (non-RT; fine here). If running, call `asioMessage(kAsioResetRequest)` rather than changing
  the clock under the host.
- `createBuffers`: validate channel indices and `bufferSize`, allocate two halves per
  requested channel (zeroed), fill `bufferInfos[n].buffers[0..1]`, keep `callbacks`. Ask the
  host `asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo)` and then
  `asioMessage(kAsioSupportsTimeInfo)` to choose `bufferSwitchTimeInfo`. Call
  `engine.ResetState()`.
- `getChannelInfo`: names and type as above; `isActive` reflects `createBuffers`; group 0.
- `future`: `kAsioCanTimeInfo` → `ASE_SUCCESS`; everything else → `ASE_InvalidParameter`.
- `outputReady()` → `ASE_NotPresent` (the loopback reads the previous half, so it isn't needed).
- `getDriverVersion()` → 1. `getErrorMessage` → last error (≤ 123 chars).
- `controlPanel()` → open the dialog (§6) and return `ASE_OK`.

## 6. Settings and the control panel

Settings live in `HKCU\Software\tonex-nam-bridge\NAMBridgeASIO`:
`ModelPath` (REG_SZ, full path), `InputGainDb`, `OutputGainDb` (REG_SZ or DWORD-encoded,
your choice), `InputSelect` (0/1/2). Use wide-char APIs and `std::filesystem::path` for the
model path: paths contain spaces, and a user's may contain non-ASCII characters. Read the
file yourself and call `LoadFromJson(json, stem)`; don't pass a narrow path to
`LoadFromFile`.

`controlPanel()` shows a **modal** Win32 dialog (it runs on the host's UI thread; audio
keeps running on the clock thread) with:

- **Model:** current model name, architecture, model rate vs host rate (and "resampling"
  when they differ), and the file path. **Load .nam…** (`GetOpenFileNameW`, filter `*.nam`)
  and **Unload** buttons. Loading goes through `LoadFromJson`; on failure show the error
  and keep the old model.
- **Input gain** and **Output gain** trackbars, −24…+24 dB in 0.5 dB steps, applied live.
- **Source channel**: Left / Right / L+R.
- **Meters:** in and out peak dBFS, refreshed by `WM_TIMER` every 100 ms via `TakePeaks`.
- A note: "In ToneX Modeler use output 1 and input 1."

Every change applies immediately and is saved to the registry. The dialog must never change
the sample rate or buffer size.

## 7. Milestone 3: `tests/asio_harness.cpp` (prove it before ToneX)

A console app that acts as an ASIO host, modeled on the macOS `tests/driver_harness.cpp`:

1. `CoInitialize`; read the CLSID from `HKLM\SOFTWARE\ASIO\NAM Bridge`; `CoCreateInstance`
   with CLSID as IID (the quirk), so this also tests registration. Optional
   `--dll <path>` mode: `LoadLibrary` + `DllGetClassObject`, so it can run before
   `regsvr32`.
2. Check `init`, `getDriverName == "NAM Bridge"`, channels 2/2, buffer sizes, the four
   rates accepted and others rejected, float32 channel type.
3. Write the test model path to the registry settings first (or add a test-only hook), so
   `init` loads it.
4. At 48000 Hz with buffer 256: `createBuffers` for out 0–1 and in 0–1; `start`. In the
   callback, write `tests/vectors/di-48k.wav` into output ch0 (ch1 silent) and record input
   ch0. Run until the whole file has gone through plus a few buffers. `stop`,
   `disposeBuffers`.
5. **Exactness:** `recorded[n + bufferSize]` must equal an offline `NamEngine` render
   (same build, same block size) of the DI with max diff < 1e-6, and the first `bufferSize`
   samples must be silent. Also compare the aligned recording with
   `expected-clubman-ssr-od-lo-48k.wav` (≤ −80 dB, as in milestone 1).
6. **Timing:** record QPC at each callback. Mean period within 0.5% of `256/48000`; report
   max jitter and any period over 2× nominal (overruns). Report the clock thread's CPU time.
7. Repeat at 44100 Hz: must run and stay level-consistent (RMS within 0.5 dB of the 48 kHz
   run). Don't expect bit-exactness, per §2.
8. Hot swap: while running, load the model again and unload it from another thread. No
   crash, and silence while unloaded.

Print `[ ok ]`/`[FAIL]` lines like the macOS harness, and exit nonzero on any failure.

## 8. Milestone 4: ToneX Modeler

1. `scripts\install-asio.ps1` (elevated `regsvr32`). Re-run the harness through the registry.
2. Open TONEX Modeler → audio settings → driver **ASIO** → **NAM Bridge**, 48 kHz if
   offered. Its driver-settings / control-panel button should open our dialog. Load the test
   model there.
3. Run a normal capture: output 1, input 1, amp-only (the Clubman file has no cab). During
   the test signal the dialog's In meter should move. If it stays at −inf, ToneX is playing on
   output 2: set Source channel to Right.
4. Let it train (the owner's RTX 2080 Super trains quickly). Report back: whether Modeler
   listed and accepted the driver, the rate and buffer size it chose, what its latency or
   reamp-calibration steps said, and how the result compares to the NAM file.

If Modeler doesn't list third-party ASIO drivers at all, stop and report. The fallback
(a WASAPI bridge between two VB-Audio Virtual Cables, like the macOS BlackHole prototype)
is a different design and needs the owner's go-ahead.

## 9. Test data

- Model: `Clubman SSR OD Lo v02 b10  t10 br10 ma12.nam` (note the **two spaces** before `t10`),
  SHA-256 `af1f8b833b0331acdcac0c6a6e592788d0020756b4c71e817d367ba738ceee2d`.
  WaveNet, 48 kHz, by "ibodog". Location: `testdata/` (git-ignored). It's the owner's
  file; don't commit it.
- `tests/vectors/di-48k.wav`: 4 s mono float32 at 48 kHz (0.25 s silence, decaying plucks
  at several levels, then a 40 Hz to 12 kHz log sweep).
- `tests/vectors/expected-clubman-ssr-od-lo-48k.wav`: NAM's reference renderer
  (`tools/render`, pinned commit) on that DI and model. The macOS engine output is
  bit-identical to it.

## 10. Don'ts

- No kernel driver or virtual audio device. ASIO only.
- Don't change the sample rate or buffer size from the driver or dialog.
- Don't allocate, lock, log or touch the registry or files on the clock thread.
- Don't export anything but the four `Dll*` functions. Keep the CRT static (already set).
- Don't modify `engine/` behavior. Portability fixes are fine; keep macOS building.
- Don't commit the ASIO SDK, the test model, or build output.
