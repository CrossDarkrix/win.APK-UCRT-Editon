# Phase 20-B2.3.6.4.3 Audio Host Link Fix

- Removes audio_decode.cpp from winapk-audio-host only.
- The isolated host consumes already-decoded float PCM and does not call clip decode().
- This removes unnecessary dependencies on stb_vorbis_decode_memory and SHCreateMemStream.
- winapk-runtime and winapk-audio-test retain audio_decode.cpp and their original link libraries.
- The libstdc++ duplicate-section message is a linker warning, not the undefined-reference failure.
