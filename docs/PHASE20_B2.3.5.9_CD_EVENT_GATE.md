# Phase 20-B2.3.5.9 CD Event Gate

- Investigation found repeated empty-drive TEST UNIT READY reached synchronous IOCTL_SCSI_PASS_THROUGH_DIRECT through JNI/BOT; REQUEST SENSE was then served from the cached native sense.
- The dominant recurring host operation is the synchronous storage IOCTL plus JNI/BOT and per-command Java logging, not PCM conversion.
- Cache sense 02/3A/00 (NOT READY / MEDIUM NOT PRESENT). Repeated TUR is answered synthetically with no storage IOCTL.
- Win32 WM_DEVICECHANGE DBT_DEVICEARRIVAL/DBT_DEVICEREMOVECOMPLETE media-volume events invalidate the cache. The first TUR after insertion/removal probes the drive physically.
- Removed normal per-CBW Java logs. Protocol error logs remain.
- Optical close reports calls, physical calls, suppressed TUR, average/max physical latency, and >=10/50 ms counts.
- USB DAC code and DISCARD completion preservation are unchanged.
