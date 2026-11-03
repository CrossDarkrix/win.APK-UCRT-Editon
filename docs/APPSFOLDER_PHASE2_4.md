# AppsFolder Phase 2-4
Native AppsFolder enumeration is exposed as synthetic Android packages named windows.app.<stable-hash>. ACTION_MAIN+CATEGORY_LAUNCHER, installed package/application queries, labels and icons are bridged. ActivityHost consumes the synthetic ComponentName and launches the original Shell item with ShellExecuteExW. Runtime log tag: Win.apk.AppsFolder.
