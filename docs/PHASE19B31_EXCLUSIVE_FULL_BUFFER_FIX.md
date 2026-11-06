# Phase 19-B3.1 Exclusive Full-Buffer Render Fix

For event-driven exclusive rendering, every signaled processing pass requests and overwrites the complete endpoint buffer returned by GetBufferSize. GetCurrentPadding is now used only for shared rendering. WAIT_TIMEOUT and failed waits never touch the render buffer. The first three exclusive submissions log event, requested frames, full-buffer byte count, and completed ReleaseBuffer submission. The Phase 19-B3 format negotiation and packed PCM24 conversion remain unchanged.
