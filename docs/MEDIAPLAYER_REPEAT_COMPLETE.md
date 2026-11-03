# MediaPlayer repeat completion

Repeat is now resilient when enabled after playback starts or after the native voice has already reached end-of-stream. Native player state tracks started/paused status and recreates a finished voice at frame zero when looping is enabled. Java starts completion monitoring when looping or a completion listener is installed after start, and reads the current listener at completion time.
