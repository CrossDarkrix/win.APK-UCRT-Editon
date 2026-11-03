// android.app.HandoffActivityData -- the return type of
// Activity.onHandoffActivityDataRequested (API 37). A TYPE-ONLY placeholder,
// for the same reason as HandoffActivityDataRequestInfo: an override names it
// in its signature, and reflection over the overriding class must resolve it.
// Nothing here ever asks an app for handoff data, so no member is mirrored.
package android.app;

public final class HandoffActivityData {
    private HandoffActivityData() {}
}
