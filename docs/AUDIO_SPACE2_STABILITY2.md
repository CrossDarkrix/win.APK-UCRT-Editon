# Audio Space2 + Stability2

- Keeps sub-110 Hz side width unchanged and boosts low Mid by 1.12x.
- Widens 110-450 Hz Side by 1.08x and upper Side by 1.12x.
- Uses phase-coherent one-pole crossover states, no Haas delay or polarity inversion.
- Applies 0.84 pre-trim before existing Punch 3 and soft limiter.
- Requests a two-period WASAPI exclusive event buffer and falls back to one device period if rejected.
- Existing endpoint prefill remains active before IAudioClient::Start.
- Stream queue minimum increases from 200 ms to 400 ms.
- Logs actual WASAPI buffer frames and milliseconds.
- SSL runtime guard, Punch 3 and CPU priority +2 remain intact.
