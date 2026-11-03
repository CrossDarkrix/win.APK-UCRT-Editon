// android.app.HandoffActivityDataRequestInfo -- the argument of
// Activity.onHandoffActivityDataRequested (API 37, cross-device task handoff).
// A TYPE-ONLY placeholder: no member of the AOSP class is mirrored, because
// Mac.apk never requests a handoff and so never builds one. It exists because
// an app activity that overrides the callback names this type in a method
// signature, and reflection over that class (getDeclaredMethod / getMethods,
// which resolve every declared signature) throws NoClassDefFoundError without
// it -- Chrome 155's activity base does exactly that, which failed every
// lifecycle callback of ChromeTabbedActivity.
package android.app;

public final class HandoffActivityDataRequestInfo {
    private HandoffActivityDataRequestInfo() {}
}
