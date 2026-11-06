# Phase 20-B2.3.4.4 URB Pool Epoch Recovery

Purpose:
Prevent late completions from an old playback source from being returned after UCRT replaces its URB pointer pool, including same-rate changes between MP3, FLAC, WAV, CD and local playback.

Implementation:
- Track current, candidate and retired URB pointer sets independently of sample rate and file format.
- Establish the initial pool normally.
- After the pool is established, three distinct new pointers while old-generation work remains are treated as a pool epoch change.
- Stop/release only the USB Direct WASAPI sink, cancel and quarantine old pending entries, clear the completion queue and increment generation.
- Promote the candidate pointer set to the new current pool.
- Recreate the sink lazily from the first valid new-generation submission.
- REAP returns only pending entries belonging to the current generation; retired old pointers are never returned as new-generation completions.
- Existing sample-rate/alternate-setting hard recovery and USB DAC/CD lifecycle separation remain enabled.
- CD BOT and HostUsbCdBridge state are not reset.

Primary diagnostic:
`Phase20-B2.3.4.4: URB pool epoch recovery reason=pointer-pool-change ...`
