# MediaPlayer repeat build fix

The repeat fallback now clears the existing `mUserPaused` state field. References to nonexistent `paused` and `stoppedByUser` variables were removed.
