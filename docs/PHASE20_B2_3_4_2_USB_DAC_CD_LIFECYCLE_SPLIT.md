# Phase 20-B2.3.4.2 USB DAC / CD lifecycle split

Problem reproduced:
- Music is first played through the virtual USB DAC.
- Starting CD playback makes UCRT close the source-side USB connection.
- The previous implementation invalidated the virtual DAC connection object and stopped its request worker.
- CD sector reads continued through HostUsbCdBridge, while the DAC route could no longer accept playback until switching to the headphone route.

Implementation:
- Virtual USB DAC close is now a soft source detach.
- The virtual DAC handle, asynchronous request worker, WASAPI route and URB ownership remain alive across music/CD source replacement.
- Optical close remains exclusively routed to HostUsbCdBridge.
- Physical non-optical USB close remains exclusively routed to HostUsbDirectBridge.
- Added explicit ownership logs for all three close paths.
- Existing rate handoff, CD BOT, READ CD, eject, +18 dB gain, pacing and destructive error/reset handling are unchanged.

Expected transition:
1. USB DAC music playback.
2. Start CD playback.
3. UCRT source close is logged as preserved virtual DAC lifecycle.
4. CD READ CD continues and 44.1 kHz PCM is delivered through the existing DAC route without switching to headphone mode.
