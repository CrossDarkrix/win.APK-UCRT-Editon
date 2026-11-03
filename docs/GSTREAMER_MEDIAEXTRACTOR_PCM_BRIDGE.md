# GStreamer MediaExtractor PCM bridge
NativeMedia.nativeDemux* now opens the path with GStreamer and publishes one audio track. Samples are decoded S16LE PCM blocks from appsink. NativeMedia.nativeDec* is a PCM pass-through so the existing MediaExtractor -> MediaCodec -> AudioTrack path remains intact. This directly satisfies PlayerController audio-track selection without modifying the UCRT APK.
