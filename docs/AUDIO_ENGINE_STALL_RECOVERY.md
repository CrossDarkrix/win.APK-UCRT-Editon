# Audio engine stall recovery

Fixes a MODE_STREAM deadlock where the native ring reaches capacity while the host mixer stops visiting Stream::mix(). Non-blocking AudioTrack.write() then returned zero forever. Stream now records a mixer heartbeat and, only after 500 ms with a completely full active ring, advances queued audio by elapsed real time until the host render loop resumes. Blocking waits are timed so recovery can be evaluated. No diagnostic file logging is included.
