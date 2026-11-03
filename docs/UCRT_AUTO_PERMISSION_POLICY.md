# UCRT automatic permission compatibility policy

The UCRT Edition grants declared Android runtime permissions without opening a
permission dialog. The existing asynchronous `onRequestPermissionsResult` path
is retained, so applications receive normal callback timing. Media/storage
requests use the pre-S5 compatibility path, and declared all-files/media access
uses the compatibility answer. Normal permissions such as INTERNET already need
no runtime prompt. Notification permission requests are treated like other
declared non-storage runtime permissions by this runtime.

This is a compatibility-oriented desktop runtime policy, not Android's standard
consent model. Set `MACAPK_MEDIA_PERMS_ASK=1` and
`MACAPK_ALLFILES_HONEST=1` before a direct runtime launch to restore interactive
storage permission handling for diagnostics. Camera, microphone and location
remain subject to their dedicated host/OS access paths.
