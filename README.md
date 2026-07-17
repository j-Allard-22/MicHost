# MicHost

A minimal Windows tray app that captures a microphone over shared-mode WASAPI,
runs it through a serial VST plugin chain in-process, and renders the result
directly into a virtual cable (VB-Cable *CABLE Input*). Discord, the Xbox PC
app, Game Bar — anything that takes a mic — then select *CABLE Output* as
their microphone.

The mic and the cable are two independently-clocked devices; MicHost
rate-matches them with a fractional resampler steered by the ring-buffer fill
level, so clock drift never turns into periodic clicks. A watchdog pins your
chosen devices across reboots, USB unplugs and sleep/wake, and the tray icon
shows health at a glance: **green** = processing on your chosen devices,
**amber** = running but wrong device/sample rate/feedback risk, **red** = no
device (waiting for it to reappear).

Free software under the GNU AGPLv3 (see `LICENSE`); the corresponding source is
at <https://github.com/j-Allard-22/MicHost>. Built on [JUCE 8](https://juce.com).

## Quick start

1. **Install [VB-Cable](https://vb-audio.com/Cable/)** (run its installer as
   admin, reboot if asked). Donationware, free for personal use.
2. **Pin everything to 48 kHz** — the single most important reliability step.
   VB-Cable defaults to 44.1 kHz; many mics (GoXLR included) are fixed at 48:
   - `mmsys.cpl` → Recording → your mic → Properties → Advanced → 48000 Hz
   - Same for Playback → *CABLE Input* and Recording → *CABLE Output*
   - Uncheck "Allow applications to take exclusive control" on *CABLE Output*
     so voice apps can't seize it.
   (MicHost survives mismatched rates — the resampler absorbs them — but
   matched rates avoid Windows' own hidden resampling on the endpoints.)
3. **Install plugins** (all free):
   - **TDR Nova** and **TDR VOS SlickEQ** (VST3) — [Tokyo Dawn Records](https://www.tokyodawn.net/tokyo-dawn-labs/)
   - **LoudMax** (VST3) — copy `LoudMax.vst3` from
     [Thomas Mundt's page](https://loudmax.blogspot.com/) into
     `C:\Program Files\Common Files\VST3`
   - **ReaPlugs** (ReaGate/ReaEQ/ReaComp/ReaFir) — [reaper.fm/reaplugs](https://www.reaper.fm/reaplugs/).
     These are VST2, so they need a VST2-enabled build of MicHost (see
     Building below); the distributed MicHost is VST3-only.
4. **Run MicHost**: pick your mic as **input** and *CABLE Input* as
   **output**; "Manage Plugins..." → add your plugin folders → Scan (scanning
   runs in a separate process, so a crashing plugin can't take MicHost down);
   "Add..." to build the chain top-to-bottom, e.g.
   gate → denoise → EQ → compressor → limiter (ceiling ≈ −1 dBFS).
5. Optional — **NVIDIA Broadcast denoise in front**: Broadcast is *not* a
   VST; it exposes a virtual "Microphone (NVIDIA Broadcast)" device. Point
   Broadcast at the physical mic, then select the Broadcast virtual mic as
   MicHost's **input**.
6. In Discord / Xbox app / Game Bar, select *CABLE Output* as the microphone.
7. Enable **"Start with Windows"** in MicHost once everything works; it will
   start hidden in the tray at login.

### Voice-app checklist (or the chain gets re-processed)

Discord and Windows both post-process mic input by default, which stacks
badly on a tuned chain (their AGC undoes your compressor; Krisp on top of
spectral denoise produces artifacts):

- Discord → Voice & Video: disable Krisp/noise suppression, echo
  cancellation, automatic gain control, and "Automatically determine input
  sensitivity" (set a manual threshold).
- Windows → Sound → Communications tab: "Do nothing".
- Keep the CABLE endpoints' volume sliders at 0 dB / 100%.

### If Windows blocks the exe

An unsigned, unknown binary can be blocked by SmartScreen ("Windows protected
your PC" → More info → Run anyway) or hard-blocked by **Smart App Control**
on Windows 11 machines where it is enabled (SAC has no per-app exceptions;
it can only be turned off system-wide in Windows Security → App & browser
control — a one-way switch, so it's your call). Building from source on the
same machine does not bypass SAC either.

### Diagnostics

- `MicHost.exe --list-devices` prints every capture/render device to stdout
  and `MicHost-devices.txt`, then exits.
- `MicHost.exe --minimized` starts hidden in the tray (the autostart toggle
  registers exactly this).
- Log file: `%APPDATA%\MicHost\MicHost.log` — device changes, watchdog
  actions, xruns, and one bridge-telemetry line per minute
  (`fill`, `corr` in ppm — the measured clock drift — `underruns/overruns`).
  Include it in bug reports.
- Settings: `%APPDATA%\MicHost\MicHost.settings` (device choices, scanned
  plugin list, the chain with each plugin's saved state). Copy it to another
  machine to migrate the chain; if a plugin is missing there, its slot is
  kept greyed-out with settings preserved rather than dropped.

## Soak-testing a new machine

Run overnight via login autostart, then check the log: watchdog lines should
show clean recovery (or nothing), cumulative xruns should stay ≈ 0, and the
bridge `corr` should settle to a stable constant (that constant is your
hardware's clock drift; ±200 ppm is unremarkable). For ground truth, record
*CABLE Output* in Audacity for a couple of hours and listen/scan for
discontinuities — counters can't prove the audio is clean, only a recording
can.

## Building from source

Prerequisites:

- Visual Studio 2022 Build Tools (C++ workload) and CMake ≥ 3.22
- JUCE 8.0.14 cloned at `external/JUCE`:
  `git clone --depth 1 --branch 8.0.14 https://github.com/juce-framework/JUCE.git external/JUCE`

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target MicHost
# -> build\MicHost_artefacts\Release\MicHost.exe
```

The exe links the CRT statically — no VC++ redistributable needed on target
machines.

**VST2 (optional, personal use only):** place the legacy VST2 SDK headers at
`sdks/vst2/pluginterfaces/vst2.x/{aeffect.h, aeffectx.h}` before configuring
and VST2 hosting is enabled automatically. Steinberg closed VST2 licensing in
2018: the headers must never be committed or redistributed (`sdks/` is
gitignored), and **any build you distribute must be VST3-only** — build it in
a separate tree with `-DMICHOST_DISABLE_VST2=ON` (see `package.ps1`, which
also zips the release). `package.ps1` runs under either Windows PowerShell 5.1
or PowerShell 7 — its only modern cmdlet, `Compress-Archive`, ships with
Windows 11, so no separate PowerShell 7 install is required.

## Known limitations

- Plugins are assumed stereo-capable (true of ReaPlugs, TDR, LoudMax); mono
  mics are fanned out to both channels before the chain.
- An Xbox *console* can never receive this audio over software — that needs a
  hardware loopback (e.g. mixer line-out → controller). The Xbox *PC app*
  works fine.
- The binary is unsigned; see "If Windows blocks the exe" above.

## Licensing

- **MicHost**: GNU AGPL-3.0-or-later (see `LICENSE`). The complete corresponding
  source for any distributed binary is at <https://github.com/j-Allard-22/MicHost>.
- **JUCE 8**: AGPLv3 / commercial dual license; this project uses it under
  AGPLv3.
- **VST3 SDK**: bundled with JUCE, GPLv3-compatible dual license (MIT as of
  SDK 3.8.0) — fine for this use.
- **VST2 headers**: not included, not licensable for distribution; local
  personal builds only, as described above.
