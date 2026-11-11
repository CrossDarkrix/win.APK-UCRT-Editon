# Phase 20-B2.3.6.4.21 Real Visualizer Tap

Built from Phase 20-B2.3.6.4.20.

## Symptom

Audio dropouts are largely fixed, but the spectrum analyser looks
steppy/starved when playing through speakers (ordinary AudioTrack
output). The user notes this predates the .15-.20 work. USB-DAC
playback looks smooth.

## Root cause

Upstream Mac.apk's `android.media.audiofx.Visualizer` is an honest
stub (its own header comment says so): `getWaveForm` fills the 0x80
silence value, `getFft` fills zeros, and the data-capture listener
**never fires**, because "the mixer has no per-session tap". An app
playing through speakers has no other Android API for spectrum data;
an app playing through this fork's USB DAC taps its own pipeline
instead -- which is exactly the observed asymmetry, and why the
problem is older than the dropout work.

## Fix

A real output tap, at the one place all audible output passes:

- `windows/host/audio_engine.cpp`: a lock-free mono ring (8192
  frames) fed from the end of `Mixer::mix` with the final clamped mix
  exactly as rendered, plus:
  - `visualizer_waveform`: latest N samples as unsigned 8-bit PCM
    (0x80 = silence), AOSP semantics.
  - `visualizer_fft`: iterative radix-2 DFT over the latest N
    samples (N = 128..2048, power of two), emitted in AOSP's FFT byte
    layout ([Re(DC), Re(Nyquist), Re/Im pairs]), scaled so a
    full-scale sine peaks near magnitude 128.
  - `visualizer_rate`: the mixer's output rate feeding the tap.
- `windows/host/native_audio.cpp`: JNI bridges
  `nativeVizGetWaveForm` / `nativeVizGetFft` /
  `nativeVizGetSamplingRateMilliHz` on `com.macapk.loader.NativeAudio`.
- `runtime/java/com/macapk/loader/NativeAudio.java`: declarations.
- `runtime/java/android/media/audiofx/Visualizer.java`: serves real
  data from the natives; `setDataCaptureListener` now actually
  delivers, via a daemon thread polling the tap at the requested rate
  (max 20 Hz per the API), on the capture thread as AOSP does.
  `setCaptureSize` validates power-of-two 128..1024 per AOSP.

Session handling: this host has exactly one audio session
(`AudioTrack.getAudioSessionId()` returns 0) and Android defines
session 0 as the output mix -- precisely what the tap records, so no
per-session routing is needed. During USB-DAC playback the app does
not use AudioTrack and the tap simply stays silent, same as before.

## Honest uncertainty

Whether UCRT's SpectrumTap reads the Visualizer in speaker mode is
inferred from the architecture (it is the only Android API for the
job, and the USB path demonstrably uses an app-internal tap), not
from a speaker-mode log. If the analyser is still steppy with this
build, a speaker-mode `last-run.log` will show which classes/APIs
the app actually exercises and the hunt continues from evidence.

## Verification

`audio_engine.cpp` / `native_audio.cpp` pass the stub-header syntax
check; `Visualizer.java` + `NativeAudio.java` compile with javac
against each other; an offline harness fed a 1 kHz full-scale sine
through the tap math and recovered the peak at bin 21/22 (1024-point
FFT at 48 kHz, true bin 21.3) with magnitude ~127 and a silent-input
waveform of pure 0x80. The cumulative patch applies cleanly to the
pristine .14 tree and reproduces this tree exactly.
