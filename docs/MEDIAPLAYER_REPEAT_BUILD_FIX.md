# MediaPlayer repeat build fix

The completion watcher now checks the current `onCompletion` field before starting. The obsolete local variable `l` was removed when listener lookup was changed to happen at completion time, but one guard still referenced it.
