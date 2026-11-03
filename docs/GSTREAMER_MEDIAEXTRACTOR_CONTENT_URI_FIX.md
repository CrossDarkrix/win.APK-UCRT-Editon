# MediaExtractor content URI fix
MediaExtractor.setDataSource(Context, Uri, headers) previously handled only file:// paths and silently returned for content://, leaving trackCount at zero. It now opens every non-file URI through ContentResolver.openAssetFileDescriptor and forwards the descriptor slice into the existing GStreamer PCM extractor.
