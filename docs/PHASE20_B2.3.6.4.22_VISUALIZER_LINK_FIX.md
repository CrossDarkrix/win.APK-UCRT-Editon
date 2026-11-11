# Phase 20-B2.3.6.4.22 Visualizer Link Fix

Built from Phase 20-B2.3.6.4.21.

## Symptom

MSYS2 UCRT64 link of `winapk-runtime.exe` fails:

```
undefined reference to `winapk::audio::visualizer_waveform(unsigned char*, int)'
undefined reference to `winapk::audio::visualizer_fft(signed char*, int)'
undefined reference to `winapk::audio::visualizer_rate()'
```

## Root cause

`audio_engine.cpp` wraps most of its implementation (Source, Mixer,
Stream, Voice) in an **anonymous namespace** inside
`namespace winapk::audio`. The .21 Visualizer reader functions were
inserted just before `class Mixer`, i.e. *inside* that anonymous
namespace, which gives them internal linkage -- the exported
`winapk::audio::visualizer_*` symbols therefore did not exist, while
the header declarations and `native_audio.cpp`'s calls expected them.
The stub-header syntax check used for verification cannot see this
(it never links), which is how it slipped through.

## Fix

Moved the three reader definitions (`visualizer_rate`,
`visualizer_waveform`, `visualizer_fft`) below the anonymous
namespace's closing brace, next to the other exported API
implementations (`set_output_endpoint`, `session_volume`, ...). They
still reach the tap state through `viz::snapshot` / `viz::rate`,
which remain inside the anonymous namespace and are visible in the
enclosing scope. The tap itself (`viz::feed` from `Mixer::mix`) is
unchanged.

## Verification (now includes a link-level check)

Compiled `audio_engine.cpp` to an object file and inspected symbols:

```
T winapk::audio::visualizer_fft(signed char*, int)
T winapk::audio::visualizer_rate()
T winapk::audio::visualizer_waveform(unsigned char*, int)
```

All three are exported (`T`) with exactly the signatures the linker
demanded. The cumulative patch applies cleanly to the pristine .14
tree and reproduces this tree.
