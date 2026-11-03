# Japanese UI refresh fix

The launcher previously changed the language flags and repainted the Settings sheet, but its Section and Row strings had already been materialized in the previous language. `apply_language()` now rebuilds the active sheet from its builder, clears stale hit targets and layout measurements, resets scrolling, and repaints it. Additional visible Settings, log, folder, shortcut, and empty-library labels are translated into Japanese.
