# Audio engine final-frame drain fix

The linear-interpolation loop previously required two queued frames. A final single complete frame could therefore never be rendered or counted in the playback head. Clients waiting for `headFrames >= framesWritten` remained at the end of the progress bar and never repeated. The mixer now renders the last frame directly when no look-ahead frame exists and consumes it. The prior full-ring stall recovery remains included. No file diagnostics are included.
