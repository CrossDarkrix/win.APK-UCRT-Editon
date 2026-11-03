from pathlib import Path
r=Path(__file__).resolve().parents[1]
c={"runtime/java/com/macapk/loader/WindowsAppsBridge.java":["query(Intent","windowsAppIconArgb","windowsAppLaunch"],"runtime/java/com/macapk/loader/MacApkPackageManager.java":["appendPackages","appendResolve","WindowsAppsBridge.icon"],"runtime/java/com/macapk/loader/ActivityHost.java":["WindowsAppsBridge.launch(intent)"],"windows/host/native_cocoa.cpp":["shell:AppsFolder","SHCreateItemWithParent","IShellItemImageFactory","ShellExecuteExW"]}
for f,ts in c.items():
 s=(r/f).read_text(encoding="utf-8")
 for t in ts:
  assert t in s,(f,t)
print("AppsFolder Phase 2-4 validation: PASS")

for token in ["ICON_EXEC","prefetchIcons","ICON_MISSES"]: assert token in (r/"runtime/java/com/macapk/loader/WindowsAppsBridge.java").read_text()
for token in ["g_icon_cache","SHGetFileInfoW","DrawIconEx"]: assert token in (r/"windows/host/native_cocoa.cpp").read_text()
print("AppsFolder async icon cache validation: PASS")

for token in ["windowsStartApps","initializeCatalog","windowsAppIconArgbAumid","countMatched"]: assert token in (r/"runtime/java/com/macapk/loader/WindowsAppsBridge.java").read_text()
for token in ["Get-StartApps","Cocoa_windowsStartApps","shell:AppsFolder\\","Cocoa_windowsAppIconArgbAumid"]: assert token in (r/"windows/host/native_cocoa.cpp").read_text()
print("AppsFolder AUMID preload validation: PASS")
