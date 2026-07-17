# MicHost

A minimal Windows tray app that captures a microphone (GoXLR) over shared-mode
WASAPI, runs it through a serial VST2/VST3 plugin chain in-process, and renders
the result directly into a virtual cable (VB-Cable Input). Discord / the Xbox
PC app then select "CABLE Output" as their microphone. This replaces the
OBS "Monitor and Output" routing hack with a purpose-built direct-render host.

Built on JUCE 8 (vendored in `external/JUCE`). Personal-use project — see
Licensing below before distributing anything.

## Build

Prerequisites (already set up if you used the bootstrap in this repo):

- Visual Studio 2022 Build Tools with the C++ workload
- CMake ≥ 3.22 (portable copy in `../tools/cmake`)
- JUCE 8.x cloned at `external/JUCE` (`git clone --depth 1 --branch 8.0.14
  https://github.com/juce-framework/JUCE.git external/JUCE`)
- Optional, for VST2 plugins (ReaPlugs): VST2 SDK headers at
  `sdks/vst2/pluginterfaces/vst2.x/{aeffect.h, aeffectx.h}` — see Licensing

```powershell
& "..\tools\cmake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "..\tools\cmake\bin\cmake.exe" --build build --config Release --target MicHost
# -> build\MicHost_artefacts\Release\MicHost.exe
```

Diagnostic mode (no GUI): `MicHost.exe --list-devices` prints every WASAPI
capture/render device to stdout and `MicHost-devices.txt`, then exits.

Start hidden in the tray: `MicHost.exe --minimized` (the in-app
"Start with Windows" toggle registers exactly this).

## First-time setup

1. Install [VB-Cable](https://vb-audio.com/Cable/) (run its installer as admin,
   reboot if asked). It is donationware and free for personal use.
2. **Pin everything to 48 kHz** — this is the single most important reliability
   step. GoXLR endpoints are fixed at 48 kHz; VB-Cable defaults to 44.1 kHz and
   silently resamples (the classic source of crackle):
   - `mmsys.cpl` → Recording → your GoXLR mic → Properties → Advanced → 48000 Hz
   - Same for Playback → *CABLE Input* and Recording → *CABLE Output*
   - Also uncheck "Allow applications to take exclusive control" on
     *CABLE Output* so party chat can't seize it.
3. Run MicHost: pick your GoXLR mic as **input** and *CABLE Input* as
   **output** on the right; "Manage Plugins..." → scan your VST folders;
   "Add..." to build the chain top-to-bottom.
4. In Discord / Xbox app / Game Bar, select *CABLE Output* as the microphone.

## Voice-app checklist (or the chain gets re-processed)

Discord and Windows both post-process mic input by default, which stacks badly
on a tuned chain (their AGC undoes your compressor; Krisp on top of spectral
denoise produces artifacts):

- Discord → Voice & Video: disable Krisp/noise suppression, echo cancellation,
  automatic gain control, and "Automatically determine input sensitivity"
  (set a manual threshold).
- Windows → Sound → Communications tab: "Do nothing".
- Keep the CABLE endpoints' volume sliders at 0 dB / 100%.
- Aim the chain's limiter ceiling around −1 dBFS.

## Known limitations (v0.1)

- **Clock drift**: the mic and the cable are independent clocks; JUCE pairs
  them with a FIFO and does not rate-match. Expect a possible click every few
  minutes-to-hours depending on hardware drift. The status bar's xrun counter
  makes this measurable — soak-test it. A fractional resampler is the
  designed fix (see roadmap).
- Plugin scanning runs in-process: a crashing plugin can take the app down
  during a scan (the crashed plugin is blacklisted on next start via the
  dead-man's-pedal file).
- Mono mics are fanned out to both chain channels; plugins are assumed
  stereo-capable (true of ReaPlugs, TDR, LoudMax).
- An Xbox *console* can never receive this audio over software — that needs a
  GoXLR line-out → controller aux loopback (hard boundary, don't chase it).

## Licensing

- **JUCE 8**: AGPLv3 / commercial dual license. Personal, non-distributed use
  is fine under AGPLv3.
- **VST3 SDK**: MIT as of 3.8.0 (bundled with JUCE) — clean for any use.
- **VST2 headers** (`sdks/vst2/`): Steinberg closed VST2 licensing in 2018.
  Hosting VST2 plugins locally for personal use is accepted practice, but the
  headers **must not be committed to any repository or redistributed** —
  `sdks/` is gitignored for that reason. If you ever distribute this app,
  build it VST3-only (delete `sdks/vst2` and the app degrades gracefully).
