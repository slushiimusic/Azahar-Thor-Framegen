# Azahar — AYN Thor patches (frame generation, dual-screen, frame pacing)

Azahar 2126.0 with seven patches aimed at making 3DS emulation feel like real
hardware on the [AYN Thor](https://www.ayntec.com/) (Snapdragon 8 Gen 2,
Adreno 740, 1080x1920 main + 1080x1240 secondary display).

Every claim below is measured on-device, not estimated. Where a change was tried
and made things worse, that is recorded too.

## What's here

| Patch | Effect |
|---|---|
| Secondary display MAILBOX | Fixes emulation-thread stalls (up to 873 ms, taking audio with them) caused by the second screen |
| Pipeline cache warm-up | Moves shader compilation into the loading screen; hitch window 50s → 18s |
| Display-sync pacing | Paces the emulator to the panel's *measured* refresh rate, not its advertised one |
| Per-window refresh pin | System stays at 120 Hz; Azahar pins its own window to 60 |
| In-process LSFG frame generation | 59 fps → **118 fps on screen**, 2.00x, GPU on its lowest clock |
| Frame generation settings + DLL picker | Normal Graphics settings, gated on Lossless.dll |
| Black frame insertion | Optional, off by default (works, but 60 Hz flicker) |

## Results (Mario Kart 7, 5x internal resolution, in-race)

```
emulator swapchain    59.01 fps
LSFG overlay         118.00 fps    p50 = p95 = p99 = max = 8.47 ms
ratio                3170 unique -> 6340 posted = 2.00x
GPU                  61% on the LOW 401 MHz clock, 73 C
dropped              2 captures in 156,601
```

## Frame generation requires Lossless.dll

LSFG extracts its shaders from `Lossless.dll`, which ships with
[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/),
commercial software. **`Lossless.dll` is not redistributable and is never
included**, in this repository or in any build. Pick it via Graphics → Frame
Generation, which copies it into the Azahar user directory.

Without it the Frame Generation toggle stays disabled and shows the reason, and
the capture never arms, so the emulator behaves exactly like upstream.

### `liblsfg-android.so`

Not in this repository either. Where you get it depends on how you install:

- **Prebuilt APK** (see Releases): the library is bundled, so frame generation
  works as soon as you supply `Lossless.dll`.
- **Building from source**: drop your own copy into
  `src/android/app/src/main/jniLibs/arm64-v8a/` before building. Without it the
  build succeeds and frame generation reports itself unavailable.

## Building

```
JAVA_HOME=/opt/homebrew/opt/openjdk@21
ANDROID_HOME=~/Library/Android/sdk        # NDK 27.0.12077973, platform 35
cd src/android && ./gradlew assembleVanillaRelease
```

Full clone with submodules — a source tarball has empty submodules and cannot build.

## Two display facts specific to this panel

Android advertises 60.000004 / 120.00001 Hz. Measured from 5,412 SurfaceFlinger
present timestamps, the **real** rates are **59.56786 Hz** and **118.01 Hz**, and
neither is a multiple of the 3DS's 59.8312 Hz.

At 120 Hz that 1.4% error puts only 54.3% of frames on the intended beat. At
60 Hz vsync absorbs the 0.45% error and the same scene is 100.0% uniform with
zero hitches. Hence: pace to the measured rate, and pin the refresh per window.

`cmd display set-user-preferred-display-mode` is not a workaround — it registers
the preference but the panel never switches, and changing modes under a running
session blanks the secondary display until restart.

## Things that did NOT work (measured, not assumed)

- **Moving the frame limiter next to the present.** Sound diagnosis, passed
  adversarial review, and the limiter genuinely does sit a frame of variable work
  upstream of the present. Measured in-race: off-beat frames went 6% → 41-51%,
  ~7x worse. The sleep holds an acquired Frame from the 3-deep pool and starves
  the next `GetRenderFrame()`. Reverted.
- **Chasing true 59.83 fps.** This panel has no ~119.66 Hz mode, so 2x59.83 is
  unpresentable; forcing it drops ~0.83 real frames/second.
- **Frame Generation Quality Mode** (36-tap flow kernels). Overlay falls
  117.94 → 89.34 fps, 154 dropped frames vs 5, GPU boosts off its low clock.
- **A Choreographer-based vsync pacer.** `postFrameCallback` re-posted from its
  own callback silently skips vsyncs undetectably, and a 240-sample window
  straddles panel mode changes — it read 60.03 / 67.67 / 62.14 Hz against a true
  59.568 and drove emulation to 113% speed. Use
  `vkGetRefreshCycleDurationGOOGLE` if a vsync clock is ever needed.

## A measurement trap worth knowing

`dumpsys SurfaceFlinger --latency` returns a 128-frame ring, which wraps in
1.08 s at 118 fps. Polling slower than that silently loses frames and reports
them as long gaps. It fabricated "26 stalls of 50-76 ms" that stayed at exactly
26 across three different fixes — that invariance is the tell. Poll at <=0.3 s
for any >100 fps layer.

## An upstream bug found along the way (not fixed here)

`src/audio_core/dsp_interface.cpp` compares `perf_stats.emulation_speed <= 95`,
but that value is a *fraction* (`perf_stats.cpp` sets it to `us/1e6`, ~0.99).
The guard has been dead since the commit that added it, so the audio
time-stretcher is permanently engaged (~125 ms latency) whenever
`enable_audio_stretching` is on. Not fixed here because on a rig deliberately
paced ~1.4% slow, the stretcher is what absorbs that deficit — disabling it
trades the latency for roughly 64 amplitude-freeze discontinuities per second.

## Licence

GPLv2, same as [Azahar](https://github.com/azahar-emu/azahar).

`Lossless.dll` is never included and remains the property of its authors. The
prebuilt APK on the Releases page does bundle `liblsfg-android.so`; the source
tree does not, so a source build needs you to supply it.
