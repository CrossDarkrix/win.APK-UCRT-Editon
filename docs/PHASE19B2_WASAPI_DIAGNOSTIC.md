# Phase 19-B2 WASAPI diagnostic

Writes diagnostic records to stderr so they are captured by Win.apk last-run.log. It reports mixer start, endpoint open and format, exclusive probe HRESULT, exclusive Initialize HRESULT, shared fallback HRESULT, renderer start mode, first AudioTrack PCM write, first WASAPI buffer submission, and HRESULT failures. Prefix: `[I] Win.apk.WASAPI: [Phase19-B2]`.
