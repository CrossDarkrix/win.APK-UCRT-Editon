# Phase 20-B2.3.3.1 Transparent Soft Limiter Fix

- 0 dB fully bypasses USB Direct gain and limiter DSP.
- Below or equal to 0.90, gained samples pass unchanged.
- Above 0.90, only the excess is smoothly compressed toward a 0.98 ceiling.
- Keeps the 100 ms gain ramp for positive gain settings.
- Diagnostics include input peak, post-gain peak, output peak, gain reduction dB, limited sample counts, and bypass state.
- Does not alter endpoint/master volume or normal Android audio.
