# UCRT connected GStreamer bridge
Registers com.uyatame.ucrt.player.WinApkGStreamerAudio and decodes the complete in-memory MP3/AAC/M4A stream through appsrc, decodebin and appsink. PCM remains S16LE and returns through UCRT's PcmDecoder interface. The UCRT archive contains a pure-Kotlin fallback for non-Win.apk environments.
