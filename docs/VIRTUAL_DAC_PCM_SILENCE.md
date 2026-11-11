# Virtual DAC PCM Silence Fill

- Detects a virtual USB DAC isochronous OUT URB with no complete stereo 24-bit PCM frame.
- Keeps the stream clock alive by synthesizing one URB interval of zero-valued stereo float PCM.
- Uses a fixed-point frame accumulator for fractional-rate packet cadence such as 44.1 kHz.
- Never overwrites a non-empty URB.
- Marks packet completion lengths for the synthesized interval and preserves normal REAP/DISCARD behavior.
- Resets the fractional phase at stream/pool/session boundaries.
- Logs fill URBs, frames, bytes, rate and cumulative totals.
