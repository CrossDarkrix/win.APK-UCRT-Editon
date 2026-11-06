# Phase 20-B2.3.3.4 Windows Adaptive 75-120 FPS

Based on B2.3.3.3 URB Rate Sync Fix. Audio and USB code are byte-identical to that known sound-correct build.

Windows renderer changes:
- Removes the fixed Sleep(16) loop and hardcoded Display.getRefreshRate()=60.
- Reads the active Windows display mode with EnumDisplaySettingsW.
- Default target is the panel refresh when 75-120 Hz, capped at 120 Hz. A 60 Hz panel remains 60 Hz because Windows cannot visibly present 75 frames on it.
- WINAPK_MAX_FPS=75..240 can explicitly override the default for diagnostics.
- Uses a high-resolution waitable timer when available and deadline-based pacing to avoid cumulative Sleep drift.
- Display.getRefreshRate(), Choreographer interval, and native render cadence share one target.
- Logs measured pacer rate, late ticks, and worst lateness every five seconds.

This build does not claim that the attached old startup log proves actual FPS; it adds direct measurement for the next run.
