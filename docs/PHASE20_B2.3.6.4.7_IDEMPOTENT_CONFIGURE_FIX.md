# Phase 20-B2.3.6.4.7 Idempotent Configure Fix

## Root cause

p20_start is called for every accepted audio URB. The previous audio-host configure method therefore set Stopped, moved read_pos to write_pos, and re-entered Running for every URB. Each call discarded PCM that had just been queued, so the host process ran but playback could not accumulate or start.

## Fix

- configure now returns immediately when state, session, generation, rate, and alt are unchanged.
- The shared PCM ring is flushed only for a real session/generation/rate/alt transition.
- host_failed causes the in-process fallback path to be selected.
- A diagnostic is emitted only for actual configuration changes.
