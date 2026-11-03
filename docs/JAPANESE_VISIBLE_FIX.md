# Japanese GUI visible fix

The launcher now loads the persisted Language setting during WM_CREATE before any visible strings or cached rows are created. The translation dispatcher also reads the persisted setting directly, eliminating dependency on global initialization order. Selecting Japanese updates the live preference before rebuilding the settings sheet and main window.
