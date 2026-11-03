# D2D-5.1 Final Polish

- Successful runtime exit is no longer misclassified as “closed before it showed a window”. Non-zero exits still show the error and log. Explicit Stop remains distinct.
- Added Japanese as a persisted launcher language selection: Windows default, English, Hebrew, Japanese. Windows-default mode selects Japanese for a Japanese Windows UI language.
- Added Japanese translations for primary library, Store, settings, install, update, removal, status and log surfaces. Missing keys safely fall back to English. Hebrew RTL remains unchanged.
