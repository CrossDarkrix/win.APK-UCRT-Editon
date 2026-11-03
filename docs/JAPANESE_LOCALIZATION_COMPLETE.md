# Japanese localization completion

The launcher now uses one authoritative `UiLanguage` state for English, Hebrew, and Japanese. Startup detection and live selection update the same state, and translation lookup reads that state directly. All strings used by the Settings builder now have Japanese translations, while unknown strings continue to fall back to English.
