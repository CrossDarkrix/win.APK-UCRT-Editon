package android.content.pm.verify.domain;

/** android.content.pm.verify.domain.DomainVerificationManager (API 31) -- the
 *  system service through which an app reads its own APP LINK verification
 *  state. Obtained via
 *  Context.getSystemService(DOMAIN_VERIFICATION_SERVICE).
 *
 *  Mirrors frameworks/base/core/java/android/content/pm/verify/domain/
 *  DomainVerificationManager.java (android16-release). The public surface is a
 *  single method; everything else upstream is @SystemApi (the verifier agent's
 *  side).
 *
 *  ⚠ MAC.APK RUNS NO DOMAIN VERIFICATION AGENT. Nothing here fetches
 *  /.well-known/assetlinks.json, nothing records a per-host state, and the
 *  intent resolver matches `<data android:scheme="https">` filters without
 *  consulting any verification state. So no package has a verification record,
 *  and AOSP's own answer for a package with none is NULL
 *  ("@return the user state, or null if the package does not declare any
 *  domains"). That is what we return -- not an invented "verified" state, which
 *  would tell an app its links are approved when nothing approved them. */
public final class DomainVerificationManager {
    public DomainVerificationManager() {}
    public DomainVerificationManager(android.content.Context context) {}

    /** AOSP DomainVerificationManager.getDomainVerificationUserState(String):
     *  "@throws PackageManager.NameNotFoundException if the given package does
     *  not exist on the device", @Nullable otherwise.
     *
     *  The not-installed branch is real -- it consults the same PackageManager
     *  every other lookup uses -- so a caller can still tell "no link state"
     *  from "no such package", which is the distinction the exception exists
     *  for. */
    public DomainVerificationUserState getDomainVerificationUserState(String packageName)
            throws android.content.pm.PackageManager.NameNotFoundException {
        android.app.Application app = android.app.Application.macapk_get();
        android.content.pm.PackageManager pm = (app != null) ? app.getPackageManager() : null;
        if (pm == null) {
            throw new android.content.pm.PackageManager.NameNotFoundException(packageName);
        }
        pm.getApplicationInfo(packageName, 0);   // throws when not installed
        return null;
    }
}
