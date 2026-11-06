# Phase 19-B3.2 Exclusive App Volume Persistence + Software Gain Fix

- Exclusive WASAPI output now applies the persisted app volume and mute as software gain before integer PCM packing.
- App-visible volume/mute values remain stable while exclusive mode has no ISimpleAudioVolume service.
- Returning to shared mode continues to synchronize with the Windows session volume.
- Diagnostics report endpoint geometry, exclusive format selection, expected byte rate, buffer size, software gain, sample peak/mean/nonzero count, payload hash/first bytes, submission cadence, and effective sample rate.
- Diagnostic output is bounded to the first few submissions and does not create a separate log file.
