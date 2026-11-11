# Phase 20-B2.3.6.4.8 Isolated Queue Fix

## Root cause

Phase 6.4.5 correctly released g_p20_stream when audio-host became the exclusive owner. However p20_enqueue_pcm still rejected every job when stream was null. The first USB SUBMIT therefore returned EIO (errno 5), set the virtual session to failed, and all later SUBMIT calls returned EPIPE (errno 32). This exactly matches the UCRT log.

## Fix

- The PCM queue now accepts a null in-process stream when audio-host is ready, Running, not failed, and owns the same generation.
- PCM remains rejected if neither runtime nor audio-host owns the route.
- Worker owner-loss diagnostics were added.
- Exclusive WASAPI ownership, warmup, idempotent configuration, fallback, and staging are preserved.
