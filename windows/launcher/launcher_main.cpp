// launcher_main.cpp -- Win.apk.exe, the app library.
//
// The Windows counterpart of upstream's launcher (launcher/*.swift), laid out
// the way upstream's SwiftUI window is: a centred title, a drop zone that
// installs whatever is dropped on (or chosen through) it, and a searchable
// grid of the installed apps with their icons. A gear opens the settings
// sheet, right-click > Get Info opens an app's sheet. The window is drawn
// with GDI+ and follows Windows' light or dark mode (or the one picked in
// the settings), in English or, laid out right to left, in Hebrew.
//
// An APK dropped on the window, chosen in the drop zone, double-clicked in
// Explorer (the launcher registers itself for .apk files) or passed on the
// command line is installed, one at a time:
//
//   1. copied into %LOCALAPPDATA%\Win.apk\library, so the app stays when the
//      download is deleted;
//   2. installed and converted once by winapk-runtime --install-only, which
//      also writes the app's icon and name (apps\<name>\icon.png, label.txt;
//      the name moves next to the APK as library\<name>.label, because the
//      app's first run moves files it does not know out of apps\<name>);
//   3. given a Start menu shortcut (Start > Win.apk Apps) and, unless turned
//      off in the settings, a desktop shortcut, with the app's own icon, that
//      start it through this program.
//
// Double-clicking an app runs it (winapk-runtime --apk, without a console);
// if it stops with an error the log can be opened. The screen size and
// orientation picked in the settings reach the runtime as WINAPK_PHONE_SIZE.
//
// The Store tab, next to the library, is F-Droid's catalog (f-droid.org,
// free and open-source apps): its index is fetched in the background and
// kept as store\catalog.tsv, searchable and by category. Install downloads
// the APK, checks it against the index's SHA-256 and installs it as above;
// an app the library already has (by package) is updated in place.
//
//   Win.apk.exe                      the library
//   Win.apk.exe <app.apk>            install, then run it
//   Win.apk.exe --run <library apk>  run an installed app (what shortcuts do)
//   Win.apk.exe install --apk <file> --source android [--requestor-label <app>] --json
//   Win.apk.exe uninstall --package <name> --source android [--requestor-label <app>] --json
//
// The last two are the install helper, upstream's macapk-installhelper: an app
// that installs apps (F-Droid, through PackageInstaller) hands its APK to
// PackageInstallerBackend, which runs this program. It asks the user, installs
// as above, and keeps installed_apps.json, the installed-apps registry that
// PackageManager answers apps from, so a store sees what it installed. Its
// output keeps the helper's contract: "[helper] progress <fraction> <stage>"
// lines, then one JSON object; exit 0 done, 2 bad arguments, 3 declined,
// 5 failed, 6 not an app.

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Gdiplus::Color;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::RectF;
using Gdiplus::SizeF;

constexpr UINT kInstalled = WM_APP + 1;       // wParam: ok | run after << 1 | running << 2, lParam: new std::wstring (the name)
constexpr UINT kRunExited = WM_APP + 2;       // wParam: ok, lParam: new std::wstring (the app's name)
constexpr UINT kInstallStarted = WM_APP + 3;  // lParam: new std::wstring (the app's name)
constexpr UINT kRefresh = WM_APP + 4;         // the install helper changed the library
constexpr UINT kStoreCatalog = WM_APP + 5;    // wParam: kCatalogList (lParam: new std::vector<StoreApp>), Done, Failed
constexpr UINT kStoreProgress = WM_APP + 6;   // wParam: KB of the catalog received so far
constexpr UINT kStoreIcon = WM_APP + 7;       // wParam: kIconFailed, Saved, Dropped; lParam: new std::string (package)
constexpr UINT kStoreDownload = WM_APP + 8;   // wParam: percent, lParam: new std::string (package)
constexpr UINT kStoreDownloaded = WM_APP + 9; // wParam: ok, lParam: new Download
constexpr UINT_PTR kAnimTimer = 1;
constexpr int kIdSearch = 100;
enum Cmd { kCmdOpen = 200, kCmdInfo, kCmdShortcut, kCmdShowApk, kCmdAppFolder, kCmdLog, kCmdReinstall, kCmdUpdate,
           kCmdRemove };

HINSTANCE g_inst;
HWND g_main, g_search;
HFONT g_edit_font, g_mono_font;
HBRUSH g_edit_brush, g_code_brush;
float g_dpi = 96;

float px(float v) { return v * g_dpi / 96.0f; }
int ipx(float v) { return static_cast<int>(std::lround(px(v))); }

// ---------------------------------------------------------------------------
// Language: Windows default, English, Hebrew RTL, or Japanese

enum class UiLanguage { English, Hebrew, Japanese };
UiLanguage g_ui_language = UiLanguage::English;
DWORD g_language_pref = 0; // 0 Windows, 1 English, 2 Hebrew, 3 Japanese
int g_live_language = -1; // -1: use persisted preference; otherwise 0..3
bool g_rtl = false;    // Hebrew: every window is mirrored (WS_EX_LAYOUTRTL) and its text reads right to left
bool g_japanese = false; // kept for compatibility with existing paint paths
float g_paint_w = 0;   // the width being painted, to put text and pictures back in a mirrored layout

DWORD setting(const wchar_t* name, DWORD fallback);

void pick_language() {
    const DWORD pref = g_live_language >= 0
        ? static_cast<DWORD>(g_live_language)
        : (std::min)(setting(L"Language", 0), 3ul);
    g_language_pref = pref;
    const LANGID ui = GetUserDefaultUILanguage();
    const bool hebrew = pref == 2 || (pref == 0 && PRIMARYLANGID(ui) == LANG_HEBREW);
    const bool japanese = pref == 3 || (pref == 0 && PRIMARYLANGID(ui) == LANG_JAPANESE);
    g_ui_language = japanese ? UiLanguage::Japanese : (hebrew ? UiLanguage::Hebrew : UiLanguage::English);
    g_rtl = g_ui_language == UiLanguage::Hebrew;
    g_japanese = g_ui_language == UiLanguage::Japanese;
}

// The Hebrew for an English string, keyed by the English itself.
const wchar_t* tr(const wchar_t* en) {
    static const std::map<std::wstring, const wchar_t*> he = {
        {L"Run Android apps natively on Windows", L"הרצת אפליקציות Android ישירות ב-Windows"},
        {L"Drop to install", L"שחרר כדי להתקין"},
        {L"Win.apk keeps its own copy, so the download can go", L"Win.apk שומרת עותק משלה, כך שאפשר למחוק את ההורדה"},
        {L"Installing…", L"מתקין…"},
        {L"Installing {0} of {1}…", L"מתקין {0} מתוך {1}…"},
        {L"Installing {0} of {1}", L"מתקין {0} מתוך {1}"},
        {L"Installing", L"מתקין"},
        {L"Unpacking manifest, icon, and assets", L"פורס את המניפסט, הסמל והמשאבים"},
        {L"{0}  ·  the first install of an app takes up to a minute", L"{0}  ·  ההתקנה הראשונה של אפליקציה נמשכת עד דקה"},
        {L"Drop an APK here to install", L"גרור לכאן קובץ APK כדי להתקין"},
        {L"or click to choose a file  ·  .apk .apkm .xapk .apks", L"או לחץ כדי לבחור קובץ  ·  apk, apkm, xapk, apks"},
        {L"Installed apps", L"אפליקציות מותקנות"},
        {L"Add app", L"הוספת אפליקציה"},
        {L"No apps match “{0}”", L"אין אפליקציות שתואמות ל„{0}”"},
        {L"Press Esc in the search box to show every app.", L"הקש Esc בתיבת החיפוש כדי להציג את כל האפליקציות."},
        {L"No apps installed yet.", L"עדיין לא הותקנו אפליקציות."},
        {L"Drop an APK above, or get apps from the Store, then open them from here.", L"גרור קובץ APK לאזור שלמעלה, או הורד אפליקציות מהחנות, ואז פתח אותן מכאן."},
        {L"Logs", L"לוגים"},
        {L"Stop", L"עצירה"},
        {L"Drop an APK above to install your first app", L"גרור קובץ APK לאזור שלמעלה כדי להתקין את האפליקציה הראשונה"},
        {L"Double-click to open  ·  right-click for options  ·  drop an APK above to add more", L"לחיצה כפולה לפתיחה  ·  לחיצה ימנית לאפשרויות  ·  גרירת APK למעלה להוספה"},
        {L"{0} apps are running", L"{0} אפליקציות פועלות"},
        {L"{0} is running", L"{0} פועלת"},
        {L"{0} app", L"אפליקציה אחת"},
        {L"{0} apps", L"{0} אפליקציות"},
        {L"The log may say why:\n{0}", L"ייתכן שהלוג מסביר למה:\n{0}"},
        {L"Open the log", L"פתיחת הלוג"},
        {L"Android apps", L"אפליקציות Android"},
        {L"All files", L"כל הקבצים"},
        {L"Install an Android app", L"התקנת אפליקציית Android"},
        {L"Installing {0}…", L"מתקין את {0}…"},
        {L"{0} is still being installed.", L"{0} עדיין בהתקנה."},
        {L"{0} is already open.", L"{0} כבר פתוחה."},
        {L"Opening {0}…", L"פותח את {0}…"},
        {L"Remove {0}?", L"להסיר את {0}?"},
        {L"Its shortcuts go too. What the app saved stays, so installing it again picks up where it left off.", L"גם קיצורי הדרך שלה יוסרו. מה שהאפליקציה שמרה נשאר, כך שהתקנה מחדש תמשיך מאותה נקודה."},
        {L"Remove", L"הסרה"},
        {L"{0} was removed.", L"{0} הוסרה."},
        {L"Update {0} from an APK", L"עדכון {0} מקובץ APK"},
        {L"Done", L"סיום"},
        {L"Theme", L"ערכת צבעים"},
        {L"Match Windows", L"כמו ב-Windows"},
        {L"Light", L"בהירה"},
        {L"Dark", L"כהה"},
        {L"Appearance", L"מראה"},
        {L"Language", L"שפה"},
        {L"Open Android app files with Win.apk", L"פתיחת קובצי אפליקציות Android עם Win.apk"},
        {L"Default", L"ברירת מחדל"},
        {L"Make default", L"הגדרה כברירת מחדל"},
        {L"File types", L"סוגי קבצים"},
        {L"Covers .apk, .apkm, .xapk and .apks. Double-clicking one in Explorer installs it and opens it.", L"כולל קובצי apk, apkm, xapk ו-apks. לחיצה כפולה על קובץ כזה בסייר הקבצים מתקינה אותו ופותחת אותו."},
        {L"Screen size", L"גודל מסך"},
        {L"Auto (fits the screen)", L"אוטומטי (מותאם למסך)"},
        {L"320 × 480  (small, older apps)", L"320 × 480  (קטן, לאפליקציות ישנות)"},
        {L"360 × 640  (5\" phone)", L"360 × 640  (טלפון 5 אינץ')"},
        {L"360 × 780  (6.5\" phone)", L"360 × 780  (טלפון 6.5 אינץ')"},
        {L"480 × 800  (6\" HDPI)", L"480 × 800  (‏6 אינץ' HDPI)"},
        {L"540 × 960  (large phone)", L"540 × 960  (טלפון גדול)"},
        {L"600 × 960  (7\" tablet)", L"600 × 960  (טאבלט 7 אינץ')"},
        {L"800 × 1280  (10\" tablet)", L"800 × 1280  (טאבלט 10 אינץ')"},
        {L"Orientation", L"כיוון"},
        {L"Portrait", L"לאורך"},
        {L"Landscape", L"לרוחב"},
        {L"Screen", L"מסך"},
        {L"Used the next time an app opens, from here or from its shortcut. Sizes are in dp, as on a phone.", L"יחול בפעם הבאה שאפליקציה תיפתח, מכאן או מקיצור הדרך שלה. הגדלים ביחידות dp, כמו בטלפון."},
        {L"Desktop shortcut for new apps", L"קיצור דרך בשולחן העבודה לאפליקציות חדשות"},
        {L"Every app also gets one in Start > Win.apk Apps.", L"כל אפליקציה מקבלת גם קיצור דרך בתפריט התחל > Win.apk Apps."},
        {L"Shortcuts", L"קיצורי דרך"},
        {L"Make the Start menu shortcuts of every app again, with their icons.", L"יצירה מחדש של קיצורי הדרך בתפריט התחל לכל האפליקציות, עם הסמלים שלהן."},
        {L"Recreate", L"יצירה מחדש"},
        {L"The Start menu shortcuts were made again.", L"קיצורי הדרך בתפריט התחל נוצרו מחדש."},
        {L"Diagnostics", L"אבחון"},
        {L"install-<app>.log records each install, last-run.log the app opened last.", L"הקובץ install-<app>.log מתעד כל התקנה, והקובץ last-run.log את האפליקציה שנפתחה אחרונה."},
        {L"View logs", L"הצגת הלוגים"},
        {L"Logs folder", L"תיקיית הלוגים"},
        {L"Open folder", L"פתיחת התיקייה"},
        {L"Attach the log to a bug report: it says what the runtime did and where it stopped.", L"צרף את הלוג לדיווח על באג: הוא מראה מה סביבת ההרצה עשתה והיכן נעצרה."},
        {L"Saves, caches and files", L"שמירות, מטמון וקבצים"},
        {L"Use my Windows folders", L"שימוש בתיקיות של Windows"},
        {L"Apps save to and open from your Downloads, Pictures, Documents, Music and Videos. Off: each folder is empty "
         L"for apps again, and your files stay where they are.",
         L"אפליקציות שומרות ופותחות קבצים מהתיקיות 'הורדות', 'תמונות', 'מסמכים', 'מוזיקה' ו'סרטונים'. כבוי: התיקיות "
         L"ריקות שוב בשביל האפליקציות, והקבצים שלך נשארים במקומם."},
        {L"Each installed app keeps its data here. Removing an app keeps what it saved.", L"כל אפליקציה מותקנת שומרת כאן את הנתונים שלה. הסרת אפליקציה משאירה את מה שהיא שמרה."},
        {L"App data", L"נתוני אפליקציות"},
        {L"Version {0}", L"גרסה {0}"},
        {L"An unofficial port of Mac.apk to Windows. Not affiliated with or endorsed by the Mac.apk project.", L"הסבה לא רשמית של Mac.apk ל-Windows. אינה קשורה לפרויקט Mac.apk ואינה מאושרת על ידו."},
        {L"About", L"אודות"},
        {L"Remove all apps", L"הסרת כל האפליקציות"},
        {L"Their shortcuts go too. What they saved stays.", L"גם קיצורי הדרך שלהן יוסרו. מה שהן שמרו נשאר."},
        {L"Remove all…", L"הסרת הכול…"},
        {L"Remove all", L"הסרת הכול"},
        {L"Remove all {0} apps?", L"להסיר את כל {0} האפליקציות?"},
        {L"Every app was removed.", L"כל האפליקציות הוסרו."},
        {L"Settings", L"הגדרות"},
        {L"{0}  ·  Android app", L"{0}  ·  אפליקציית Android"},
        {L"Identity", L"זיהוי"},
        {L"Name", L"שם"},
        {L"Library name", L"שם בספרייה"},
        {L"Installed", L"הותקנה"},
        {L"Storage", L"אחסון"},
        {L"Source APK  ·  {0}", L"קובץ ה-APK המקורי  ·  {0}"},
        {L"App files  ·  {0}", L"קבצי האפליקציה  ·  {0}"},
        {L"Show", L"הצגה"},
        {L"Open", L"פתיחה"},
        {L"Install log", L"לוג ההתקנה"},
        {L"View", L"הצגה"},
        {L"Open {0}", L"פתיחת {0}"},
        {L"Desktop shortcut", L"קיצור דרך בשולחן העבודה"},
        {L"Put a shortcut to the app on the desktop.", L"הוספת קיצור דרך לאפליקציה בשולחן העבודה."},
        {L"Create", L"יצירה"},
        {L"A shortcut to {0} is on the desktop.", L"קיצור דרך ל-{0} נמצא בשולחן העבודה."},
        {L"Update from APK", L"עדכון מקובץ APK"},
        {L"Install a newer version over this one. Saves stay.", L"התקנת גרסה חדשה יותר במקום הנוכחית. השמירות נשארות."},
        {L"Choose…", L"בחירה…"},
        {L"Reinstall", L"התקנה מחדש"},
        {L"Convert the app again from its APK.", L"המרת האפליקציה מחדש מקובץ ה-APK שלה."},
        {L"Its shortcuts go too. What the app saved stays.", L"גם קיצורי הדרך שלה יוסרו. מה שהאפליקציה שמרה נשאר."},
        {L"Remove…", L"הסרה…"},
        {L"Actions", L"פעולות"},
        {L"Each app is converted for Windows once. That takes up to a minute.", L"כל אפליקציה מומרת ל-Windows פעם אחת. זה נמשך עד דקה."},
        {L"{0} is ready", L"{0} מוכנה"},
        {L"All set", L"הכול מוכן"},
        {L"{0} app is in the library and in Start > Win.apk Apps.", L"האפליקציה נמצאת בספרייה ובתפריט התחל > Win.apk Apps."},
        {L"{0} apps are in the library and in Start > Win.apk Apps.", L"{0} אפליקציות נמצאות בספרייה ובתפריט התחל > Win.apk Apps."},
        {L"The install failed", L"ההתקנה נכשלה"},
        {L"Some installs failed", L"חלק מההתקנות נכשלו"},
        {L"{0} of {1} could not be installed. The log says why.", L"{0} מתוך {1} לא הותקנו. הלוג מסביר למה."},
        {L"Waiting", L"ממתינה"},
        {L"Converting  ·  {0}", L"ממירה  ·  {0}"},
        {L"Installed in {0}", L"הותקנה תוך {0}"},
        {L"Failed after {0}", L"נכשלה אחרי {0}"},
        {L"View log", L"הצגת הלוג"},
        {L"Log  ·  {0}", L"לוג  ·  {0}"},
        {L"Log", L"לוג"},
        {L"Starting the runtime…", L"מפעיל את סביבת ההרצה…"},
        {L"The install's output appears here.", L"הפלט של ההתקנה יופיע כאן."},
        {L"Hide", L"הסתרה"},
        {L"Open full log", L"פתיחת הלוג המלא"},
        {L"Last run", L"הרצה אחרונה"},
        {L"Install  ·  {0}", L"התקנה  ·  {0}"},
        {L"No app has run yet. This log is written when one does.", L"עדיין לא הורצה אף אפליקציה. הלוג ייכתב כשאחת תורץ."},
        {L"This log is written the next time the app is installed.", L"הלוג ייכתב בפעם הבאה שהאפליקציה תותקן."},
        {L"Live", L"חי"},
        {L"Copy", L"העתקה"},
        {L"Notepad", L"פנקס רשימות"},
        {L"Folder", L"תיקייה"},
        {L"The log was copied.", L"הלוג הועתק."},
        {L"&Open", L"פתיחה"},
        {L"Get &Info…", L"מידע…"},
        {L"Create &desktop shortcut", L"יצירת קיצור דרך בשולחן העבודה"},
        {L"Show the &APK in Explorer", L"הצגת קובץ ה-APK בסייר הקבצים"},
        {L"Open the app's &files", L"פתיחת קבצי האפליקציה"},
        {L"Show the last &log", L"הצגת הלוג האחרון"},
        {L"&Update from APK…", L"עדכון מקובץ APK…"},
        {L"Rein&stall", L"התקנה מחדש"},
        {L"&Remove…", L"הסרה…"},
        {L"Search apps", L"חיפוש אפליקציות"},
        {L"Installing - Win.apk", L"התקנה - Win.apk"},
        {L"Logs - Win.apk", L"לוגים - Win.apk"},
        {L"{0} is installed: it is in the Start menu and on the desktop.", L"{0} הותקנה: היא בתפריט התחל ובשולחן העבודה."},
        {L"{0} is installed: it is in the Start menu.", L"{0} הותקנה: היא בתפריט התחל."},
        {L"Installing {0}… The first install of an app takes up to a minute.", L"מתקין את {0}… ההתקנה הראשונה של אפליקציה נמשכת עד דקה."},
        {L"Installing {0} failed.", L"ההתקנה של {0} נכשלה."},
        {L"Installing {0} failed", L"ההתקנה של {0} נכשלה"},
        {L"{0} was stopped.", L"{0} נעצרה."},
        {L"{0} stopped with an error.", L"{0} נעצרה עם שגיאה."},
        {L"{0} stopped with an error", L"{0} נעצרה עם שגיאה"},
        {L"{0} closed before it showed a window.", L"{0} נסגרה לפני שהציגה חלון."},
        {L"{0} closed before it showed a window", L"{0} נסגרה לפני שהציגה חלון"},
        {L"What the log says", L"מה כתוב בלוג"},
        {L"This APK has no native code for this app (lib/arm64-v8a). Install the full bundle (.apkm, .xapk or .apks) that includes the arm64-v8a part.",
         L"ב-APK הזה חסר הקוד המקורי של האפליקציה (lib/arm64-v8a). יש להתקין את החבילה המלאה (.apkm, .xapk או .apks) שכוללת את החלק arm64-v8a."},
        {L"The app stopped with an error.\n\nThe log is in:\n{0}", L"האפליקציה נעצרה עם שגיאה.\n\nהלוג נמצא ב:\n{0}"},
        {L"An app", L"אפליקציה"},
        {L"Close {0} before updating it.", L"יש לסגור את {0} לפני העדכון."},
        {L"{0} wants to remove {1}.\n\nRemove it?", L"{0} מבקשת להסיר את {1}.\n\nלהסיר?"},
        {L"{0} wants to update {1} ({2}).\n\nUpdate it?", L"{0} מבקשת לעדכן את {1} ({2}).\n\nלעדכן?"},
        {L"{0} wants to install {1}.\n\nInstall it?", L"{0} מבקשת להתקין את {1}.\n\nלהתקין?"},
        {L"bytes", L"בתים"},
        {L"Library", L"ספרייה"},
        {L"Store", L"חנות"},
        {L"Search F-Droid", L"חיפוש ב-F-Droid"},
        {L"All categories", L"כל הקטגוריות"},
        {L"Install", L"התקנה"},
        {L"Update", L"עדכון"},
        {L"Getting the app catalog from F-Droid…", L"מוריד את קטלוג האפליקציות מ-F-Droid…"},
        {L"{0} MB so far. This happens once; later the store opens at once.", L"{0} MB עד עכשיו. זה קורה פעם אחת; בהמשך החנות תיפתח מיד."},
        {L"Checking F-Droid for new apps…", L"בודק אם יש אפליקציות חדשות ב-F-Droid…"},
        {L"F-Droid could not be reached", L"לא ניתן להתחבר ל-F-Droid"},
        {L"Check the internet connection, then try again.", L"בדוק את החיבור לאינטרנט ונסה שוב."},
        {L"Try again", L"ניסיון חוזר"},
        {L"Try other words, or every category.", L"נסה מילים אחרות, או את כל הקטגוריות."},
        {L"Free and open-source apps from f-droid.org  ·  Install adds the app to your library", L"אפליקציות חינמיות בקוד פתוח מ-f-droid.org  ·  התקנה מוסיפה את האפליקציה לספרייה"},
        {L"Downloading {0}…", L"מוריד את {0}…"},
        {L"Downloading…", L"מוריד…"},
        {L"Downloading {0} failed. Check the internet connection, then try again.", L"ההורדה של {0} נכשלה. בדוק את החיבור לאינטרנט ונסה שוב."},
        {L"F-Droid could not be checked for new apps.", L"לא ניתן היה לבדוק אם יש אפליקציות חדשות ב-F-Droid."},
        {L"Details", L"פרטים"},
        {L"Version", L"גרסה"},
        {L"Size", L"גודל"},
        {L"Updated", L"עודכנה"},
        {L"License", L"רישיון"},
        {L"Categories", L"קטגוריות"},
        {L"Package", L"חבילה"},
        {L"Install {0}", L"התקנת {0}"},
        {L"Update {0}", L"עדכון {0}"},
        {L"Downloaded from F-Droid and checked, then added to the library.", L"מורדת מ-F-Droid ונבדקת, ואז נוספת לספרייה."},
        {L"A newer version is out. Saves stay.", L"יצאה גרסה חדשה יותר. השמירות נשארות."},
        {L"F-Droid page", L"הדף ב-F-Droid"},
        {L"The app's description, screenshots and source code.", L"התיאור של האפליקציה, צילומי מסך וקוד המקור."},
        {L"unknown", L"לא ידוע"}
    };
    if (g_ui_language == UiLanguage::Japanese) {
        if (wcscmp(en, L"bytes") == 0) return L"バイト";
        if (wcscmp(en, L"unknown") == 0) return L"不明";
        if (wcscmp(en, L"Run Android apps natively on Windows") == 0) return L"AndroidアプリをWindows上でネイティブ実行";
        if (wcscmp(en, L"Drop to install") == 0) return L"ドロップしてインストール";
        if (wcscmp(en, L"Win.apk keeps its own copy, so the download can go") == 0) return L"Win.apkがコピーを保持するため、ダウンロード元は削除できます";
        if (wcscmp(en, L"Installing…") == 0) return L"インストール中…";
        if (wcscmp(en, L"Unpacking manifest, icon, and assets") == 0) return L"マニフェスト、アイコン、アセットを展開中";
        if (wcscmp(en, L"Drop an APK here to install") == 0) return L"APKをここにドロップしてインストール";
        if (wcscmp(en, L"or click to choose a file  ·  .apk .apkm .xapk .apks") == 0) return L"またはクリックしてファイルを選択  ·  .apk .apkm .xapk .apks";
        if (wcscmp(en, L"Installed apps") == 0) return L"インストール済みアプリ";
        if (wcscmp(en, L"Add app") == 0) return L"アプリを追加";
        if (wcscmp(en, L"Press Esc in the search box to show every app.") == 0) return L"検索欄でEscを押すと、すべてのアプリを表示します。";
        if (wcscmp(en, L"No apps installed yet.") == 0) return L"インストール済みのアプリはありません。";
        if (wcscmp(en, L"Drop an APK above, or get apps from the Store, then open them from here.") == 0) return L"上にAPKをドロップするか、ストアからアプリを入手して、ここから開いてください。";
        if (wcscmp(en, L"Checking F-Droid for new apps…") == 0) return L"F-Droidで新しいアプリを確認しています…";
        if (wcscmp(en, L"All categories") == 0) return L"すべてのカテゴリ";
        if (wcscmp(en, L"Install") == 0) return L"インストール";
        if (wcscmp(en, L"Update") == 0) return L"更新";
        if (wcscmp(en, L"Open") == 0) return L"開く";
        if (wcscmp(en, L"Waiting") == 0) return L"待機中";
        if (wcscmp(en, L"Getting the app catalog from F-Droid…") == 0) return L"F-Droidからアプリ一覧を取得しています…";
        if (wcscmp(en, L"F-Droid could not be reached") == 0) return L"F-Droidに接続できませんでした";
        if (wcscmp(en, L"Check the internet connection, then try again.") == 0) return L"インターネット接続を確認して、もう一度お試しください。";
        if (wcscmp(en, L"Try again") == 0) return L"再試行";
        if (wcscmp(en, L"Try other words, or every category.") == 0) return L"別の語句またはすべてのカテゴリーをお試しください。";
        if (wcscmp(en, L"Logs") == 0) return L"ログ";
        if (wcscmp(en, L"Stop") == 0) return L"停止";
        if (wcscmp(en, L"Free and open-source apps from f-droid.org  ·  Install adds the app to your library") == 0) return L"f-droid.orgの無料オープンソースアプリ  ·  インストールするとライブラリに追加されます";
        if (wcscmp(en, L"Drop an APK above to install your first app") == 0) return L"上にAPKをドロップして最初のアプリをインストール";
        if (wcscmp(en, L"Double-click to open  ·  right-click for options  ·  drop an APK above to add more") == 0) return L"ダブルクリックで開く  ·  右クリックでオプション  ·  上にAPKをドロップして追加";
        if (wcscmp(en, L"What the log says") == 0) return L"ログの内容";
        if (wcscmp(en, L"Open the log") == 0) return L"ログを開く";
        if (wcscmp(en, L"Android apps") == 0) return L"Androidアプリ";
        if (wcscmp(en, L"All files") == 0) return L"すべてのファイル";
        if (wcscmp(en, L"Install an Android app") == 0) return L"Androidアプリをインストール";
        if (wcscmp(en, L"Its shortcuts go too. What the app saved stays, so installing it again picks up where it left off.") == 0) return L"ショートカットも削除されます。保存データは残るため、再インストールすると続きから使用できます。";
        if (wcscmp(en, L"Remove") == 0) return L"削除";
        if (wcscmp(en, L"Done") == 0) return L"完了";
        if (wcscmp(en, L"Theme") == 0) return L"テーマ";
        if (wcscmp(en, L"Match Windows") == 0) return L"Windowsに合わせる";
        if (wcscmp(en, L"Light") == 0) return L"ライト";
        if (wcscmp(en, L"Dark") == 0) return L"ダーク";
        if (wcscmp(en, L"Language") == 0) return L"言語";
        if (wcscmp(en, L"Appearance") == 0) return L"外観";
        if (wcscmp(en, L"Open Android app files with Win.apk") == 0) return L"AndroidアプリファイルをWin.apkで開く";
        if (wcscmp(en, L"Default") == 0) return L"既定";
        if (wcscmp(en, L"Make default") == 0) return L"既定にする";
        if (wcscmp(en, L"File types") == 0) return L"ファイルの種類";
        if (wcscmp(en, L"Covers .apk, .apkm, .xapk and .apks. Double-clicking one in Explorer installs it and opens it.") == 0) return L".apk、.apkm、.xapk、.apksに対応します。エクスプローラーでダブルクリックするとインストールして起動します。";
        if (wcscmp(en, L"Screen size") == 0) return L"画面サイズ";
        if (wcscmp(en, L"Orientation") == 0) return L"画面の向き";
        if (wcscmp(en, L"Portrait") == 0) return L"縦向き";
        if (wcscmp(en, L"Landscape") == 0) return L"横向き";
        if (wcscmp(en, L"Screen") == 0) return L"画面";
        if (wcscmp(en, L"Used the next time an app opens, from here or from its shortcut. Sizes are in dp, as on a phone.") == 0) return L"次回アプリをここまたはショートカットから開いたときに使用します。サイズの単位はスマートフォンと同じdpです。";
        if (wcscmp(en, L"Desktop shortcut for new apps") == 0) return L"新しいアプリのデスクトップショートカット";
        if (wcscmp(en, L"Every app also gets one in Start > Win.apk Apps.") == 0) return L"各アプリはスタートメニューの「Win.apk Apps」にも追加されます。";
        if (wcscmp(en, L"Shortcuts") == 0) return L"ショートカット";
        if (wcscmp(en, L"Make the Start menu shortcuts of every app again, with their icons.") == 0) return L"すべてのアプリのスタートメニューショートカットをアイコン付きで再作成します。";
        if (wcscmp(en, L"Recreate") == 0) return L"再作成";
        if (wcscmp(en, L"The Start menu shortcuts were made again.") == 0) return L"スタートメニューのショートカットを再作成しました。";
        if (wcscmp(en, L"Diagnostics") == 0) return L"診断";
        if (wcscmp(en, L"install-<app>.log records each install, last-run.log the app opened last.") == 0) return L"install-<app>.logには各インストール、last-run.logには最後に開いたアプリの記録が保存されます。";
        if (wcscmp(en, L"View logs") == 0) return L"ログを表示";
        if (wcscmp(en, L"Logs folder") == 0) return L"ログフォルダー";
        if (wcscmp(en, L"Open folder") == 0) return L"フォルダーを開く";
        if (wcscmp(en, L"Attach the log to a bug report: it says what the runtime did and where it stopped.") == 0) return L"不具合報告にはログを添付してください。ランタイムの処理内容と停止位置を確認できます。";
        if (wcscmp(en, L"Use my Windows folders") == 0) return L"Windowsのフォルダーを使用";
        if (wcscmp(en, L"App data") == 0) return L"アプリデータ";
        if (wcscmp(en, L"Saves, caches and files") == 0) return L"保存データ、キャッシュ、ファイル";
        if (wcscmp(en, L"Each installed app keeps its data here. Removing an app keeps what it saved.") == 0) return L"インストールした各アプリのデータをここに保存します。アプリを削除しても保存データは残ります。";
        if (wcscmp(en, L"About") == 0) return L"このアプリについて";
        if (wcscmp(en, L"An unofficial port of Mac.apk to Windows. Not affiliated with or endorsed by the Mac.apk project.") == 0) return L"Mac.apkをWindowsへ移植した非公式版です。Mac.apkプロジェクトとの提携や承認関係はありません。";
        if (wcscmp(en, L"Remove all apps") == 0) return L"すべてのアプリを削除";
        if (wcscmp(en, L"Their shortcuts go too. What they saved stays.") == 0) return L"ショートカットも削除されますが、保存データは残ります。";
        if (wcscmp(en, L"Remove all…") == 0) return L"すべて削除…";
        if (wcscmp(en, L"Remove all") == 0) return L"すべて削除";
        if (wcscmp(en, L"Every app was removed.") == 0) return L"すべてのアプリを削除しました。";
        if (wcscmp(en, L"Settings") == 0) return L"設定";
        if (wcscmp(en, L"Identity") == 0) return L"識別情報";
        if (wcscmp(en, L"Name") == 0) return L"名前";
        if (wcscmp(en, L"Library name") == 0) return L"ライブラリ名";
        if (wcscmp(en, L"Installed") == 0) return L"インストール済み";
        if (wcscmp(en, L"Storage") == 0) return L"ストレージ";
        if (wcscmp(en, L"Show") == 0) return L"表示";
        if (wcscmp(en, L"Install log") == 0) return L"インストールログ";
        if (wcscmp(en, L"View") == 0) return L"表示";
        if (wcscmp(en, L"Actions") == 0) return L"操作";
        if (wcscmp(en, L"Desktop shortcut") == 0) return L"デスクトップショートカット";
        if (wcscmp(en, L"Put a shortcut to the app on the desktop.") == 0) return L"アプリのショートカットをデスクトップに作成します。";
        if (wcscmp(en, L"Create") == 0) return L"作成";
        if (wcscmp(en, L"Update from APK") == 0) return L"APKから更新";
        if (wcscmp(en, L"Install a newer version over this one. Saves stay.") == 0) return L"新しいバージョンを上書きインストールします。保存データは維持されます。";
        if (wcscmp(en, L"Choose…") == 0) return L"選択…";
        if (wcscmp(en, L"Reinstall") == 0) return L"再インストール";
        if (wcscmp(en, L"Convert the app again from its APK.") == 0) return L"APKからアプリを再変換します。";
        if (wcscmp(en, L"Its shortcuts go too. What the app saved stays.") == 0) return L"ショートカットも削除されます。保存データは残ります。";
        if (wcscmp(en, L"Remove…") == 0) return L"削除…";
        if (wcscmp(en, L"Details") == 0) return L"詳細";
        if (wcscmp(en, L"Version") == 0) return L"バージョン";
        if (wcscmp(en, L"Size") == 0) return L"サイズ";
        if (wcscmp(en, L"Updated") == 0) return L"更新済み";
        if (wcscmp(en, L"License") == 0) return L"ライセンス";
        if (wcscmp(en, L"Categories") == 0) return L"カテゴリー";
        if (wcscmp(en, L"Package") == 0) return L"パッケージ";
        if (wcscmp(en, L"Downloading…") == 0) return L"ダウンロード中…";
        if (wcscmp(en, L"F-Droid page") == 0) return L"F-Droidページ";
        if (wcscmp(en, L"The app's description, screenshots and source code.") == 0) return L"アプリの説明、スクリーンショット、ソースコード。";
        if (wcscmp(en, L"Installing") == 0) return L"インストール中";
        if (wcscmp(en, L"Each app is converted for Windows once. That takes up to a minute.") == 0) return L"各アプリはWindows向けに一度だけ変換されます。最大1分ほどかかります。";
        if (wcscmp(en, L"All set") == 0) return L"準備完了";
        if (wcscmp(en, L"The install failed") == 0) return L"インストールに失敗しました";
        if (wcscmp(en, L"Some installs failed") == 0) return L"一部のインストールに失敗しました";
        if (wcscmp(en, L"View log") == 0) return L"ログを表示";
        if (wcscmp(en, L"Log") == 0) return L"ログ";
        if (wcscmp(en, L"Starting the runtime…") == 0) return L"ランタイムを起動しています…";
        if (wcscmp(en, L"The install's output appears here.") == 0) return L"インストールの出力がここに表示されます。";
        if (wcscmp(en, L"Hide") == 0) return L"非表示";
        if (wcscmp(en, L"Open full log") == 0) return L"完全なログを開く";
        if (wcscmp(en, L"Installing - Win.apk") == 0) return L"インストール中 - Win.apk";
        if (wcscmp(en, L"Last run") == 0) return L"最終実行";
        if (wcscmp(en, L"No app has run yet. This log is written when one does.") == 0) return L"まだアプリは実行されていません。実行時にこのログへ記録されます。";
        if (wcscmp(en, L"This log is written the next time the app is installed.") == 0) return L"次回アプリをインストールしたときに、このログへ記録されます。";
        if (wcscmp(en, L"Folder") == 0) return L"フォルダー";
        if (wcscmp(en, L"Notepad") == 0) return L"メモ帳";
        if (wcscmp(en, L"Copy") == 0) return L"コピー";
        if (wcscmp(en, L"Live") == 0) return L"実行中";
        if (wcscmp(en, L"The log was copied.") == 0) return L"ログをコピーしました。";
        if (wcscmp(en, L"Logs - Win.apk") == 0) return L"ログ - Win.apk";
        if (wcscmp(en, L"&Open") == 0) return L"開く(&O)";
        if (wcscmp(en, L"Get &Info…") == 0) return L"情報を取得(&I)…";
        if (wcscmp(en, L"Create &desktop shortcut") == 0) return L"デスクトップショートカットを作成";
        if (wcscmp(en, L"Show the &APK in Explorer") == 0) return L"APKをエクスプローラーで表示(&A)";
        if (wcscmp(en, L"Open the app's &files") == 0) return L"アプリのファイルを開く(&F)";
        if (wcscmp(en, L"Show the last &log") == 0) return L"最後のログを表示";
        if (wcscmp(en, L"&Update from APK…") == 0) return L"APKから更新…";
        if (wcscmp(en, L"Rein&stall") == 0) return L"再インストール";
        if (wcscmp(en, L"&Remove…") == 0) return L"削除…";
        if (wcscmp(en, L"F-Droid could not be checked for new apps.") == 0) return L"F-Droidで新しいアプリを確認できませんでした。";
        if (wcscmp(en, L"An app") == 0) return L"アプリ";
        return en;
    }
    if (g_ui_language == UiLanguage::Hebrew) {
        const auto it = he.find(en);
        return it == he.end() ? en : it->second;
    }
    return en;
}


// tr(en) with {0}, {1}... replaced by `args`.
std::wstring fmt(const wchar_t* en, std::initializer_list<std::wstring> args) {
    std::wstring s = tr(en);
    size_t i = 0;
    for (const std::wstring& a : args) {
        const std::wstring key = L"{" + std::to_wstring(i++) + L"}";
        const size_t at = s.find(key);
        if (at != std::wstring::npos) s.replace(at, key.size(), a);
    }
    return s;
}

std::wstring app_count(size_t n) { return fmt(n == 1 ? L"{0} app" : L"{0} apps", {std::to_wstring(n)}); }
DWORD layout_style() { return g_rtl ? WS_EX_LAYOUTRTL : 0; }
// A popup's flags; `anchored` popups open under a box whose start edge is the point.
UINT menu_flags(bool anchored) {
    return g_rtl ? TPM_LAYOUTRTL | (anchored ? TPM_RIGHTALIGN : 0u) : (anchored ? TPM_LEFTALIGN : 0u);
}

// ---------------------------------------------------------------------------
// Paths

fs::path exe_path() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return fs::path(std::wstring(buf, n));
}
fs::path root() { return exe_path().parent_path(); }
fs::path runtime_exe() { return root() / L"bin" / L"winapk-runtime.exe"; }

fs::path data_dir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 4);
    return fs::path(std::wstring(buf, n)) / L"Win.apk";
}
fs::path library_dir() { return data_dir() / L"library"; }
fs::path app_dir(const std::wstring& name) { return data_dir() / L"apps" / name; }
fs::path logs_dir() { return data_dir() / L"logs"; }
fs::path last_run_log() { return logs_dir() / L"last-run.log"; }
fs::path install_log(const std::wstring& name) { return logs_dir() / (L"install-" + name + L".log"); }

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}
fs::path start_menu_apps() { return known_folder(FOLDERID_Programs) / L"Win.apk Apps"; }

const wchar_t* const kExtensions[] = {L".apk", L".apkm", L".xapk", L".apks"};

bool is_app_file(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return std::any_of(std::begin(kExtensions), std::end(kExtensions), [&](const wchar_t* e) { return ext == e; });
}

std::wstring utf8_to_wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string wide_to_utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring file_name_safe(std::wstring s) {
    for (auto& c : s) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    }
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    return s.empty() ? L"App" : s;
}

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    return s;
}

std::wstring format_bytes(uintmax_t n) {
    const wchar_t* units[] = {tr(L"bytes"), L"KB", L"MB", L"GB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024 && u < 3) v /= 1024, ++u;
    wchar_t buf[64];
    swprintf(buf, 64, u == 0 ? L"%.0f %ls" : L"%.1f %ls", v, units[u]);
    return buf;
}

uintmax_t folder_size(const fs::path& dir) {
    uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

std::wstring long_date(const FILETIME& when) {
    SYSTEMTIME utc, local;
    wchar_t buf[128] = {};
    if (!FileTimeToSystemTime(&when, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) ||
        !GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &local, nullptr, buf, 128, nullptr)) {
        return tr(L"unknown");
    }
    return buf;
}

std::wstring file_date(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) ? long_date(a.ftLastWriteTime) : tr(L"unknown");
}

// FILETIME counts 100 ns from 1601, Java's milliseconds from 1970.
constexpr unsigned long long kUnixEpochFiletime = 116444736000000000ull;

std::wstring java_date(long long ms) {
    if (ms <= 0) return tr(L"unknown");
    const unsigned long long t = kUnixEpochFiletime + static_cast<unsigned long long>(ms) * 10000;
    return long_date(FILETIME{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)});
}

// The version in launcher.rc, read from the program's own resource.
std::wstring version() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const auto* p = static_cast<const BYTE*>(mem ? LockResource(mem) : nullptr);
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    for (DWORD i = 0; p && i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
        const auto* fi = reinterpret_cast<const VS_FIXEDFILEINFO*>(p + i);
        if (fi->dwSignature != 0xFEEF04BD) continue;
        return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionMS)) +
               L"." + std::to_wstring(HIWORD(fi->dwFileVersionLS));
    }
    return L"0";
}

void open_path(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void show_in_explorer(const fs::path& p) {
    const std::wstring args = L"/select,\"" + p.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void open_log(const fs::path& log) {
    ShellExecuteW(nullptr, L"open", L"notepad.exe", log.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Settings, in HKCU\Software\Win.apk

constexpr wchar_t kSettingsKey[] = L"Software\\Win.apk";

DWORD setting(const wchar_t* name, DWORD fallback) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS
               ? v
               : fallback;
}

void set_setting(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

struct PhonePreset {
    const wchar_t* label;
    int w, h;  // dp; 0 x 0 is the runtime's own choice
};

const PhonePreset kPhonePresets[] = {
    {L"Fit screen 1270 x 720", 1270, 720},
    {L"320 × 480  (small, older apps)", 320, 480},
    {L"360 × 640  (5\" phone)", 360, 640},
    {L"360 × 780  (6.5\" phone)", 360, 780},
    {L"393 × 851  (Pixel 7)", 393, 851},
    {L"412 × 915  (Pixel 6)", 412, 915},
    {L"480 × 800  (6\" HDPI)", 480, 800},
    {L"540 × 960  (large phone)", 540, 960},
    {L"600 × 960  (7\" tablet)", 600, 960},
    {L"800 × 1280  (10\" tablet)", 800, 1280},
};

// The settings' screen size and orientation, for the runtime started from
// here (it inherits this process's environment). "Auto" leaves whatever
// WINAPK_PHONE_SIZE the user set themselves.
void apply_runtime_env() {
    // UCRT Edition compatibility policy: declared runtime permissions are
    // granted without opening the Android permission helper. This covers media,
    // shared-file access and notifications; INTERNET is a normal install-time
    // permission and already needs no runtime prompt. Set either variable to 1
    // before launching to restore the interactive storage permission flow.
    SetEnvironmentVariableW(L"MACAPK_MEDIA_PERMS_ASK", L"0");
    SetEnvironmentVariableW(L"MACAPK_ALLFILES_HONEST", L"0");
    SetEnvironmentVariableW(L"WINAPK_UCRT_AUTO_PERMISSIONS", L"1");

    const DWORD i = setting(L"PhoneSize", 0);
    const bool landscape = setting(L"Landscape", 0) != 0;
    // Fit screen is a full 1270 x 720 render surface. It also disables
    // letterboxing so every submitted source frame is scaled to the complete
    // client area whenever the user resizes the window.
    if (i == 0 || i >= std::size(kPhonePresets)) {
        SetEnvironmentVariableW(L"WINAPK_PHONE_SIZE", landscape ? L"720x1270" : L"1270x720");
        SetEnvironmentVariableW(L"MACAPK_LETTERBOX", L"0");
        return;
    }
    int w = kPhonePresets[i].w, h = kPhonePresets[i].h;
    if (landscape) std::swap(w, h);
    SetEnvironmentVariableW(L"WINAPK_PHONE_SIZE", (std::to_wstring(w) + L"x" + std::to_wstring(h)).c_str());
    // Every device preset uses full-client stretch fit. The selected
    // preset still controls the guest render resolution, while presentation,
    // pointer mapping, and child-window placement use the complete client area.
    SetEnvironmentVariableW(L"MACAPK_LETTERBOX", L"0");
}

// ---------------------------------------------------------------------------
// Windows integration: file types and shortcuts

void set_reg_default(const std::wstring& sub, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

std::wstring reg_default(const std::wstring& sub) {
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return L"";
    }
    return buf;
}

bool is_default_handler() { return reg_default(L"Software\\Classes\\.apk") == L"WinApk.Package"; }

// Claims the app file types; without force, only those no other program has.
void register_file_types(bool force) {
    const std::wstring exe = exe_path().wstring();
    set_reg_default(L"Software\\Classes\\WinApk.Package", L"Android app");
    set_reg_default(L"Software\\Classes\\WinApk.Package\\DefaultIcon", L"\"" + exe + L"\",0");
    set_reg_default(L"Software\\Classes\\WinApk.Package\\shell\\open\\command", L"\"" + exe + L"\" \"%1\"");
    for (const wchar_t* ext : kExtensions) {
        const std::wstring key = std::wstring(L"Software\\Classes\\") + ext;
        const std::wstring current = reg_default(key);
        if (force || current.empty() || current == L"WinApk.Package") set_reg_default(key, L"WinApk.Package");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool make_shortcut(const fs::path& lnk, const std::wstring& args, const fs::path& icon, const std::wstring& what) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(exe_path().c_str());
    link->SetArguments(args.c_str());
    link->SetWorkingDirectory(root().c_str());
    link->SetDescription(what.c_str());
    if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
    if (ok) {
        std::error_code ec;
        fs::create_directories(lnk.parent_path(), ec);
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

void integrate_with_windows() {
    const fs::path self = known_folder(FOLDERID_Programs) / L"Win.apk.lnk";
    std::error_code ec;
    if (!fs::exists(self, ec)) make_shortcut(self, L"", fs::path(), L"Android apps on Windows");
    register_file_types(false);
}

// ---------------------------------------------------------------------------
// The library

struct App {
    std::wstring name;   // the library file's stem: the key everywhere
    std::wstring label;  // what the user sees
    fs::path apk;
};

fs::path label_file(const std::wstring& name) { return library_dir() / (name + L".label"); }
// The app's package, versionCode and versionName, one per line (AppMeta's package.txt).
fs::path package_file(const std::wstring& name) { return library_dir() / (name + L".package"); }
fs::path shortcut_icon(const std::wstring& name) { return library_dir() / (name + L".ico"); }

std::wstring label_of(const std::wstring& name) {
    std::ifstream f(label_file(name), std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::wstring w = utf8_to_wide(s);
    return w.empty() ? name : w;
}

std::vector<App> library() {
    std::vector<App> apps;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(library_dir(), ec)) {
        if (!e.is_regular_file(ec) || !is_app_file(e.path())) continue;
        const std::wstring name = e.path().stem().wstring();
        if (!fs::exists(label_file(name), ec)) continue;  // not installed (yet)
        apps.push_back({name, label_of(name), e.path()});
    }
    std::sort(apps.begin(), apps.end(), [](const App& a, const App& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.label.c_str(), -1, b.label.c_str(), -1,
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return apps;
}

bool write_ico(const fs::path& png, const fs::path& ico) {
    std::ifstream f(png, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 24 || data[1] != 'P') return false;
    const uint32_t w = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
    const uint32_t h = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
    std::ofstream o(ico, std::ios::binary | std::ios::trunc);
    auto u16 = [&](uint16_t v) { o.put(static_cast<char>(v & 0xff)).put(static_cast<char>(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(static_cast<uint16_t>(v & 0xffff)); u16(static_cast<uint16_t>(v >> 16)); };
    u16(0), u16(1), u16(1);
    o.put(static_cast<char>(w >= 256 ? 0 : w)).put(static_cast<char>(h >= 256 ? 0 : h)).put(0).put(0);
    u16(1), u16(32), u32(static_cast<uint32_t>(data.size())), u32(22);
    o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(o);
}

std::wstring run_args(const fs::path& apk) { return L"--run \"" + apk.wstring() + L"\""; }

void make_app_shortcuts(const App& app, bool desktop) {
    const fs::path ico = shortcut_icon(app.name);
    const bool has_icon = write_ico(app_dir(app.name) / L"icon.png", ico);
    const std::wstring file = file_name_safe(app.label) + L".lnk";
    const std::wstring what = app.label + L" (Android app, Win.apk)";
    make_shortcut(start_menu_apps() / file, run_args(app.apk), has_icon ? ico : fs::path(), what);
    if (desktop) {
        make_shortcut(known_folder(FOLDERID_Desktop) / file, run_args(app.apk), has_icon ? ico : fs::path(), what);
    }
}

void remove_app_shortcuts(const App& app) {
    std::error_code ec;
    const std::wstring file = file_name_safe(app.label) + L".lnk";
    fs::remove(start_menu_apps() / file, ec);
    fs::remove(known_folder(FOLDERID_Desktop) / file, ec);
}

// ---------------------------------------------------------------------------
// Running and installing

// The runtime compiles an app's arm64 code with clang (WINAPK_AOT_CLANG, Win.apk\clang, or the PATH);
// without one it falls back to the slower JIT. A clang installed elsewhere, by MSYS2 or the LLVM
// installer, is handed over so the code conversion works without changing the PATH.
void apply_aot_clang_env() {
    wchar_t have[8];
    if (GetEnvironmentVariableW(L"WINAPK_AOT_CLANG", have, 8) > 0) return;
    std::error_code ec;
    if (fs::exists(root() / L"clang" / L"bin" / L"clang.exe", ec)) return;
    if (SearchPathW(nullptr, L"clang.exe", nullptr, 0, nullptr, nullptr) > 0) return;
    std::vector<fs::path> candidates;
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramW6432"}) {
        wchar_t dir[MAX_PATH];
        if (GetEnvironmentVariableW(var, dir, MAX_PATH) > 0) {
            candidates.push_back(fs::path(dir) / L"LLVM\\bin\\clang.exe");
        }
    }
    candidates.push_back(L"C:\\msys64\\clang64\\bin\\clang.exe");
    candidates.push_back(L"C:\\msys64\\ucrt64\\bin\\clang.exe");
    for (const auto& c : candidates) {
        if (fs::exists(c, ec)) {
            SetEnvironmentVariableW(L"WINAPK_AOT_CLANG", c.c_str());
            return;
        }
    }
}

HANDLE start_runtime(const std::wstring& args, const fs::path& log) {
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    apply_runtime_env();
    apply_aot_clang_env();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring cmd = L"\"" + runtime_exe().wstring() + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   root().c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

DWORD wait_exit(HANDLE process) {
    WaitForSingleObject(process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    return code;
}

bool run_and_wait(const fs::path& apk) {
    HANDLE p = start_runtime(L"--apk \"" + apk.wstring() + L"\"", last_run_log());
    return p && wait_exit(p) == 0;
}

struct RunJob {
    std::wstring name;
    fs::path apk;
};

SRWLOCK g_proc_lock = SRWLOCK_INIT;
std::map<std::wstring, HANDLE> g_procs;  // the runtimes of apps started from the library, for Stop

DWORD WINAPI run_thread(void* param) {
    std::unique_ptr<RunJob> job(static_cast<RunJob*>(param));
    HANDLE p = start_runtime(L"--apk \"" + job->apk.wstring() + L"\"", last_run_log());
    DWORD code = 1;
    if (p) {
        AcquireSRWLockExclusive(&g_proc_lock);
        g_procs[job->name] = p;
        ReleaseSRWLockExclusive(&g_proc_lock);
        WaitForSingleObject(p, INFINITE);
        GetExitCodeProcess(p, &code);
        AcquireSRWLockExclusive(&g_proc_lock);
        g_procs.erase(job->name);
        ReleaseSRWLockExclusive(&g_proc_lock);
        CloseHandle(p);
    }
    PostMessageW(g_main, kRunExited, code == 0, reinterpret_cast<LPARAM>(new std::wstring(job->name)));
    return 0;
}

bool stop_process(const std::wstring& name) {
    AcquireSRWLockExclusive(&g_proc_lock);
    const auto it = g_procs.find(name);
    const bool found = it != g_procs.end() && TerminateProcess(it->second, 1);
    ReleaseSRWLockExclusive(&g_proc_lock);
    return found;
}

struct InstallJob {
    fs::path source;
    std::wstring name;
    fs::path replaces;  // the app's previous APK, when it is updated from one with another extension
    bool run_after;
    bool temporary;     // the store's download, removed once it is in the library
};

SRWLOCK g_install_lock = SRWLOCK_INIT;  // one install at a time: each one is a JVM converting the whole app

void write_registry();

// Whether the app is running: winapk-runtime holds a named mutex keyed on
// the app's data folder for as long as it runs the app (claim_single_instance
// in host/runtime_main.cpp; the key is made the same way here).
bool app_running(const std::wstring& name) {
    std::error_code ec;
    std::wstring key = fs::absolute(app_dir(name), ec).lexically_normal().wstring();
    if (ec) key = app_dir(name).wstring();
    while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c == L'/' ? L'\\' : c));
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the UTF-16 units
    for (const wchar_t c : key) h = (h ^ static_cast<uint16_t>(c)) * 1099511628211ull;
    wchar_t mutex[48];
    swprintf(mutex, 48, L"Local\\WinApk-app-%016llx", static_cast<unsigned long long>(h));
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, mutex);
    if (!m) return false;
    const DWORD w = WaitForSingleObject(m, 0);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) ReleaseMutex(m);
    CloseHandle(m);
    return w == WAIT_TIMEOUT;
}

// Copies `source` into the library as <name><ext> and installs it there: the
// install, the code conversion, the name, icon and package, the shortcuts.
// `replaces` is the app's previous APK, removed once the new one is in. An
// app that is running is left alone (its files are in use): false, with
// `*running` set.
bool install_into_library(const fs::path& source, const std::wstring& name, const fs::path& replaces,
                          bool* running) {
    *running = app_running(name);
    if (*running) return false;
    std::error_code ec;
    fs::create_directories(library_dir(), ec);
    const fs::path target = library_dir() / (name + source.extension().wstring());
    bool ok = fs::equivalent(source, target, ec) || CopyFileW(source.c_str(), target.c_str(), FALSE) != 0;
    if (ok) {
        HANDLE p = start_runtime(L"--apk \"" + target.wstring() + L"\" --install-only", install_log(name));
        const DWORD code = p ? wait_exit(p) : 1;
        *running = code == 3;  // the app was started meanwhile
        ok = code == 0 &&
             MoveFileExW((app_dir(name) / L"label.txt").c_str(), label_file(name).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
        if (ok) MoveFileExW((app_dir(name) / L"package.txt").c_str(), package_file(name).c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    }
    if (ok && !replaces.empty() && replaces != target) fs::remove(replaces, ec);
    if (ok) make_app_shortcuts({name, label_of(name), target}, setting(L"DesktopShortcut", 1) != 0);
    write_registry();
    return ok;
}

DWORD WINAPI install_thread(void* param) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::unique_ptr<InstallJob> job(static_cast<InstallJob*>(param));
    AcquireSRWLockExclusive(&g_install_lock);
    PostMessageW(g_main, kInstallStarted, 0, reinterpret_cast<LPARAM>(new std::wstring(job->name)));
    bool running = false;
    const bool ok = install_into_library(job->source, job->name, job->replaces, &running);
    ReleaseSRWLockExclusive(&g_install_lock);
    std::error_code ec;
    if (job->temporary) fs::remove(job->source, ec);
    PostMessageW(g_main, kInstalled, (ok ? 1 : 0) | (job->run_after ? 2 : 0) | (running ? 4 : 0),
                 reinterpret_cast<LPARAM>(new std::wstring(job->name)));
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// The store: F-Droid's catalog, its icons and its APKs

constexpr wchar_t kRepo[] = L"https://f-droid.org/repo/";
// In 100 ns units; f-droid.org publishes a few times a day.
constexpr unsigned long long kCatalogMaxAge = 6ull * 3600 * 10000000;

fs::path store_dir() { return data_dir() / L"store"; }
fs::path catalog_file() { return store_dir() / L"catalog.tsv"; }
fs::path catalog_etag_file() { return store_dir() / L"catalog.etag"; }
fs::path store_icon_file(const std::string& pkg) { return store_dir() / L"icons" / (utf8_to_wide(pkg) + L".png"); }
fs::path downloads_dir() { return store_dir() / L"downloads"; }

struct StoreApp {
    std::string pkg, icon, apk, sha256;  // icon: a URL; apk: the file's name in the repo
    std::wstring name, summary, name_he, summary_he, version, license;
    std::vector<std::wstring> categories;
    long long code = 0, size = 0, updated = 0;  // versionCode, bytes, ms since 1970
};

// One WinHTTP session for the program, so the icons' requests reuse their
// connections to f-droid.org rather than each making its own.
HINTERNET http_session() {
    static const HINTERNET session = [] {
        HINTERNET s = WinHttpOpen(L"Win.apk", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s) {  // before Windows 8.1
            s = WinHttpOpen(L"Win.apk", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (s) {
            DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;  // the catalog is 70 MB of JSON, a fifth of that gzipped
            WinHttpSetOption(s, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
            DWORD per_server = 8;
            WinHttpSetOption(s, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &per_server, sizeof(per_server));
            WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
        }
        return s;
    }();
    return session;
}

// GETs `url` into `out`, through <out>.part so a broken download never looks
// whole. With an `etag` the server may answer 304, not modified. `progress`
// gets the bytes so far and the total (0 when unknown). Returns the HTTP
// status, 0 when the server could not be reached or the body broke off.
int http_get(const std::wstring& url, const fs::path& out, const std::wstring& etag, std::wstring* new_etag,
             const std::function<void(uint64_t, uint64_t)>& progress) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return 0;
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    const std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
                              std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    HINTERNET session = http_session();
    if (!session) return 0;
    HINTERNET connection = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                        uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                   : nullptr;
    int status = 0;
    const std::wstring header = etag.empty() ? L"" : L"If-None-Match: " + etag;
    if (request &&
        WinHttpSendRequest(request, header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header.c_str(),
                           header.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = static_cast<int>(code);
        if (status == 200) {
            DWORD total = 0;
            size = sizeof(total);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &total, &size, WINHTTP_NO_HEADER_INDEX);
            if (new_etag) {
                wchar_t tag[256] = {};
                size = sizeof(tag);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, tag, &size,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    *new_etag = tag;
                }
            }
            std::error_code ec;
            fs::create_directories(out.parent_path(), ec);
            const fs::path part = out.wstring() + L".part";
            HANDLE f =
                CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            bool ok = f != INVALID_HANDLE_VALUE;
            uint64_t got = 0;
            std::vector<char> buf(1 << 16);
            for (DWORD n = 0; ok; got += n) {
                ok = WinHttpReadData(request, buf.data(), static_cast<DWORD>(buf.size()), &n) != 0;
                if (!ok || n == 0) break;
                DWORD written = 0;
                ok = WriteFile(f, buf.data(), n, &written, nullptr) && written == n;
                if (ok && progress) progress(got + n, total);
            }
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            ok = ok && got > 0 && MoveFileExW(part.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok) {
                fs::remove(part, ec);
                status = 0;
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    return status;
}

// The file's SHA-256 in lower-case hex, as F-Droid's index gives it.
std::string sha256_of(const fs::path& file) {
    std::string out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f != INVALID_HANDLE_VALUE &&
        BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<UCHAR> buf(1 << 16);
        DWORD n = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0) {
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), n, 0));
        }
        UCHAR digest[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            for (const UCHAR b : digest) {
                char hex[3];
                std::snprintf(hex, sizeof hex, "%02x", b);
                out += hex;
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return out;
}

// A pull parser for the one JSON file the store reads, F-Droid's
// index-v1.json: it walks the 70 MB once without building a tree, and the
// callers keep only the fields the store shows.
struct Json {
    const char* p;
    const char* end;
    bool ok = true;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    bool eat(char c) {
        ws();
        if (p >= end || *p != c) return false;
        ++p;
        return true;
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            if (p >= end || !isxdigit(static_cast<unsigned char>(*p))) {
                ok = false;
                return 0;
            }
            const char c = *p;
            v = v * 16 + static_cast<unsigned>(c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        return v;
    }
    static void put_utf8(std::string* out, unsigned cp) {
        if (cp < 0x80) {
            *out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            *out += static_cast<char>(0xC0 | (cp >> 6));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            *out += static_cast<char>(0xE0 | (cp >> 12));
            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            *out += static_cast<char>(0xF0 | (cp >> 18));
            *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    // A string with its escapes decoded; `out` may be null to skip it.
    void str(std::string* out) {
        if (!eat('"')) {
            ok = false;
            return;
        }
        while (ok && p < end && *p != '"') {
            if (*p != '\\') {
                if (out) *out += *p;
                ++p;
                continue;
            }
            if (++p >= end) break;
            const char e = *p++;
            if (e == 'u') {
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    const unsigned lo = hex4();
                    cp = lo >= 0xDC00 && lo < 0xE000 ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
                }
                if (out) put_utf8(out, cp);
            } else if (out) {
                *out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
            }
        }
        if (p >= end) ok = false;
        else ++p;
    }
    // A string, number, true, false or null, as its text.
    void scalar(std::string* out) {
        ws();
        if (p < end && *p == '"') return str(out);
        const char* from = p;
        while (p < end && !strchr(",}] \n\r\t", *p)) ++p;
        if (p == from) ok = false;
        else if (out) out->assign(from, p);
    }
    template <class Each>
    void object(Each each) {
        if (!eat('{')) {
            ok = false;
            return;
        }
        if (eat('}')) return;
        std::string key;
        do {
            key.clear();
            str(&key);
            if (!ok || !eat(':')) {
                ok = false;
                return;
            }
            each(key);
        } while (ok && eat(','));
        if (ok && !eat('}')) ok = false;
    }
    template <class Each>
    void array(Each each) {
        if (!eat('[')) {
            ok = false;
            return;
        }
        if (eat(']')) return;
        do each();
        while (ok && eat(','));
        if (ok && !eat(']')) ok = false;
    }
    void skip() {
        ws();
        if (p < end && *p == '{') object([this](const std::string&) { skip(); });
        else if (p < end && *p == '[') array([this] { skip(); });
        else scalar(nullptr);
    }
    std::vector<std::string> strings() {
        std::vector<std::string> out;
        array([&] {
            out.emplace_back();
            str(&out.back());
        });
        return out;
    }
};

// One APK of an app in the index.
struct IndexApk {
    std::string file, sha256, version;
    long long code = 0, size = 0;
    bool arm_only = false;  // native code for arm64 and not x86_64: it runs, translated, but an x86_64 build is better
};

// The fields of index-v1.json's app entry the store keeps; the names and
// summaries per language come from `localized` when the entry has none of
// its own.
struct IndexApp {
    std::string pkg, name, summary, name_he, summary_he, icon, license, suggested, updated;
    std::vector<std::string> categories;
};

void read_index_app(Json& j, IndexApp* a) {
    std::string top_icon, any_icon_lang, any_icon;
    std::map<std::string, std::pair<std::string, std::string>> names;  // language: name, summary
    j.object([&](const std::string& k) {
        if (k == "packageName") j.str(&a->pkg);
        else if (k == "name") j.str(&a->name);
        else if (k == "summary") j.str(&a->summary);
        else if (k == "icon") j.str(&top_icon);
        else if (k == "license") j.str(&a->license);
        else if (k == "suggestedVersionCode") j.scalar(&a->suggested);
        else if (k == "lastUpdated") j.scalar(&a->updated);
        else if (k == "categories") a->categories = j.strings();
        else if (k == "localized") {
            j.object([&](const std::string& lang) {
                std::string name, summary, icon;
                j.object([&](const std::string& f) {
                    if (f == "name") j.str(&name);
                    else if (f == "summary") j.str(&summary);
                    else if (f == "icon") j.str(&icon);
                    else j.skip();
                });
                if (!icon.empty() && (any_icon.empty() || lang == "en-US")) any_icon_lang = lang, any_icon = icon;
                names[lang] = {name, summary};
            });
        } else {
            j.skip();
        }
    });
    auto pick = [&](std::string* out, bool summary, std::initializer_list<const char*> langs, bool any) {
        for (const char* lang : langs) {
            const auto it = names.find(lang);
            const std::string& v = it == names.end() ? std::string() : summary ? it->second.second : it->second.first;
            if (out->empty() && !v.empty()) *out = v;
        }
        for (const auto& n : names) {
            if (!any || !out->empty()) break;
            *out = summary ? n.second.second : n.second.first;
        }
    };
    pick(&a->name, false, {"en-US", "en", "en-GB"}, true);
    pick(&a->summary, true, {"en-US", "en", "en-GB"}, true);
    pick(&a->name_he, false, {"he", "iw", "he-IL", "iw-IL"}, false);
    pick(&a->summary_he, true, {"he", "iw", "he-IL", "iw-IL"}, false);
    if (!top_icon.empty()) a->icon = wide_to_utf8(kRepo) + "icons-240/" + top_icon;
    else if (!any_icon.empty()) a->icon = wide_to_utf8(kRepo) + a->pkg + "/" + any_icon_lang + "/" + any_icon;
}

// An APK entry; false when it cannot run here: native code for neither
// x86_64 nor arm64-v8a (Win.apk translates arm64), or no SHA-256 to check.
bool read_index_apk(Json& j, IndexApk* apk) {
    std::string hash_type, code, size;
    std::vector<std::string> abis;
    j.object([&](const std::string& k) {
        if (k == "apkName") j.str(&apk->file);
        else if (k == "hash") j.str(&apk->sha256);
        else if (k == "hashType") j.str(&hash_type);
        else if (k == "versionName") j.str(&apk->version);
        else if (k == "versionCode") j.scalar(&code);
        else if (k == "size") j.scalar(&size);
        else if (k == "nativecode") abis = j.strings();
        else j.skip();
    });
    apk->code = std::atoll(code.c_str());
    apk->size = std::atoll(size.c_str());
    const bool x64 = std::find(abis.begin(), abis.end(), "x86_64") != abis.end();
    const bool arm64 = std::find(abis.begin(), abis.end(), "arm64-v8a") != abis.end();
    apk->arm_only = arm64 && !x64;
    return (abis.empty() || x64 || arm64) && hash_type == "sha256" && apk->sha256.size() == 64 && !apk->file.empty();
}

std::string tsv_field(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

// Turns index-v1.json into catalog.tsv: a line per app that can run here,
// with the APK the store installs. That is F-Droid's suggested version (the
// newest, unless it marks a newer one as not ready) and, where an app is
// built per ABI, its x86_64 build. Returns the number of apps.
size_t convert_index(const fs::path& json_file, const fs::path& out) {
    std::string json;
    {
        std::ifstream f(json_file, std::ios::binary);
        json.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    Json j{json.data(), json.data() + json.size()};
    std::vector<IndexApp> apps;
    std::map<std::string, std::vector<IndexApk>> apks;
    j.object([&](const std::string& key) {
        if (key == "apps") {
            j.array([&] {
                apps.emplace_back();
                read_index_app(j, &apps.back());
            });
        } else if (key == "packages") {
            j.object([&](const std::string& pkg) {
                std::vector<IndexApk>& list = apks[pkg];
                j.array([&] {
                    IndexApk apk;
                    if (read_index_apk(j, &apk)) list.push_back(apk);
                });
            });
        } else {
            j.skip();
        }
    });
    if (!j.ok) return 0;
    std::string text = "winapk-catalog 1\n";
    size_t count = 0;
    for (const IndexApp& a : apps) {
        const auto it = apks.find(a.pkg);
        if (a.pkg.empty() || a.name.empty() || it == apks.end() || it->second.empty()) continue;
        const long long suggested = std::atoll(a.suggested.c_str());
        const IndexApk* best = nullptr;
        for (int pass = 0; pass < 2 && !best; ++pass) {  // the suggested version or older, else any
            for (const IndexApk& apk : it->second) {
                if (pass == 0 && suggested > 0 && apk.code > suggested) continue;
                if (!best || apk.code > best->code) best = &apk;
            }
        }
        if (best->arm_only) {
            for (const IndexApk& apk : it->second) {
                if (!apk.arm_only && apk.version == best->version) best = &apk;
            }
        }
        std::string cats;
        for (const std::string& c : a.categories) cats += (cats.empty() ? "" : ";") + c;
        const std::string fields[] = {a.pkg, std::to_string(best->code), std::to_string(best->size), a.updated,
                                      best->file, best->sha256, a.icon, best->version, a.license, cats,
                                      a.name, a.summary, a.name_he, a.summary_he};
        for (size_t i = 0; i < std::size(fields); ++i) text += (i ? "\t" : "") + tsv_field(fields[i]);
        text += "\n";
        ++count;
    }
    const fs::path tmp = out.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) return 0;
    }
    return count && MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING) ? count : 0;
}

std::vector<StoreApp> read_catalog() {
    std::vector<StoreApp> out;
    std::ifstream f(catalog_file(), std::ios::binary);
    std::string line;
    if (!std::getline(f, line) || line != "winapk-catalog 1") return out;
    while (std::getline(f, line)) {
        std::vector<std::string> v;
        for (size_t from = 0;;) {
            const size_t tab = line.find('\t', from);
            v.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
            if (tab == std::string::npos) break;
            from = tab + 1;
        }
        if (v.size() != 14) continue;
        StoreApp a;
        a.pkg = v[0];
        a.code = std::atoll(v[1].c_str());
        a.size = std::atoll(v[2].c_str());
        a.updated = std::atoll(v[3].c_str());
        a.apk = v[4];
        a.sha256 = v[5];
        a.icon = v[6];
        a.version = utf8_to_wide(v[7]);
        a.license = utf8_to_wide(v[8]);
        for (size_t from = 0; from < v[9].size();) {
            const size_t semi = (std::min)(v[9].find(';', from), v[9].size());
            a.categories.push_back(utf8_to_wide(v[9].substr(from, semi - from)));
            from = semi + 1;
        }
        a.name = utf8_to_wide(v[10]);
        a.summary = utf8_to_wide(v[11]);
        a.name_he = utf8_to_wide(v[12]);
        a.summary_he = utf8_to_wide(v[13]);
        out.push_back(std::move(a));
    }
    return out;
}

unsigned long long file_age(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return ~0ull;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const auto t = [](const FILETIME& f) {
        return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return t(now) > t(a.ftLastWriteTime) ? t(now) - t(a.ftLastWriteTime) : 0;
}

void touch(const fs::path& p) {
    HANDLE f = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(f, nullptr, nullptr, &now);
    CloseHandle(f);
}

enum { kCatalogList, kCatalogDone, kCatalogFailed };

// Shows the catalog kept from last time, then fetches F-Droid's index when
// that is older than kCatalogMaxAge (or `param` asks to), unless the server
// says it has not changed.
DWORD WINAPI catalog_thread(void* param) {
    const bool force = param != nullptr;
    std::error_code ec;
    fs::create_directories(store_dir(), ec);
    const bool have = fs::exists(catalog_file(), ec);
    if (have) {
        PostMessageW(g_main, kStoreCatalog, kCatalogList,
                     reinterpret_cast<LPARAM>(new std::vector<StoreApp>(read_catalog())));
    }
    int result = kCatalogDone;
    if (!have || force || file_age(catalog_file()) > kCatalogMaxAge) {
        std::wstring etag, new_etag;
        if (have) {
            std::ifstream f(catalog_etag_file(), std::ios::binary);
            std::string s;
            std::getline(f, s);
            etag = utf8_to_wide(s);
        }
        const fs::path raw = store_dir() / L"index-v1.json";
        ULONGLONG last = 0;
        const int status = http_get(std::wstring(kRepo) + L"index-v1.json", raw, etag, &new_etag,
                                    [&](uint64_t got, uint64_t) {
                                        if (GetTickCount64() - last < 100) return;
                                        last = GetTickCount64();
                                        PostMessageW(g_main, kStoreProgress, static_cast<WPARAM>(got >> 10), 0);
                                    });
        if (status == 304) {
            touch(catalog_file());
        } else if (status == 200 && convert_index(raw, catalog_file())) {
            std::ofstream(catalog_etag_file(), std::ios::binary | std::ios::trunc) << wide_to_utf8(new_etag);
            PostMessageW(g_main, kStoreCatalog, kCatalogList,
                         reinterpret_cast<LPARAM>(new std::vector<StoreApp>(read_catalog())));
        } else {
            result = kCatalogFailed;
        }
        fs::remove(raw, ec);
    }
    PostMessageW(g_main, kStoreCatalog, result, 0);
    return 0;
}

enum { kIconFailed, kIconSaved, kIconDropped };

// Icons are fetched by a few threads from a queue, the newest request first:
// the cards on screen now come before those scrolled past.
SRWLOCK g_icon_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_icon_wake = CONDITION_VARIABLE_INIT;
std::vector<std::pair<std::string, std::string>> g_icon_queue;  // package, URL
int g_icon_threads = 0;

DWORD WINAPI icon_thread(void*) {
    for (;;) {
        AcquireSRWLockExclusive(&g_icon_lock);
        while (g_icon_queue.empty()) SleepConditionVariableSRW(&g_icon_wake, &g_icon_lock, INFINITE, 0);
        const auto [pkg, url] = g_icon_queue.back();
        g_icon_queue.pop_back();
        ReleaseSRWLockExclusive(&g_icon_lock);
        const bool ok = http_get(utf8_to_wide(url), store_icon_file(pkg), L"", nullptr, {}) == 200;
        PostMessageW(g_main, kStoreIcon, ok ? kIconSaved : kIconFailed, reinterpret_cast<LPARAM>(new std::string(pkg)));
    }
}

void request_icon(const std::string& pkg, const std::string& url) {
    AcquireSRWLockExclusive(&g_icon_lock);
    g_icon_queue.emplace_back(pkg, url);
    if (g_icon_queue.size() > 48) {  // long scrolled past: asked for again if it comes back into view
        PostMessageW(g_main, kStoreIcon, kIconDropped,
                     reinterpret_cast<LPARAM>(new std::string(g_icon_queue.front().first)));
        g_icon_queue.erase(g_icon_queue.begin());
    }
    if (g_icon_threads < 4) {
        if (HANDLE t = CreateThread(nullptr, 0, icon_thread, nullptr, 0, nullptr)) {
            CloseHandle(t);
            ++g_icon_threads;
        }
    }
    ReleaseSRWLockExclusive(&g_icon_lock);
    WakeConditionVariable(&g_icon_wake);
}

struct Download {
    std::string pkg, sha256;
    std::wstring url, label;
    fs::path file;
};

SRWLOCK g_download_lock = SRWLOCK_INIT;  // one download at a time; the installs after them go one at a time too

DWORD WINAPI download_thread(void* param) {
    std::unique_ptr<Download> d(static_cast<Download*>(param));
    AcquireSRWLockExclusive(&g_download_lock);
    int shown = -1;
    const int status = http_get(d->url, d->file, L"", nullptr, [&](uint64_t got, uint64_t total) {
        const int pct = total ? static_cast<int>((std::min)(got * 100 / total, uint64_t{99})) : 0;
        if (pct == shown) return;
        shown = pct;
        PostMessageW(g_main, kStoreDownload, static_cast<WPARAM>(pct),
                     reinterpret_cast<LPARAM>(new std::string(d->pkg)));
    });
    // The APK must be the one F-Droid's index names; anything else is not installed.
    const bool ok = status == 200 && sha256_of(d->file) == d->sha256;
    std::error_code ec;
    if (!ok) fs::remove(d->file, ec);
    ReleaseSRWLockExclusive(&g_download_lock);
    PostMessageW(g_main, kStoreDownloaded, ok, reinterpret_cast<LPARAM>(d.release()));
    return 0;
}

// ---------------------------------------------------------------------------
// Theme and drawing

struct Theme {
    bool dark;
    Color bg, card, hover, border, text, text2, accent, danger, success;
};

Color hex(uint32_t v, BYTE a = 255) { return Color(a, (v >> 16) & 255, (v >> 8) & 255, v & 255); }
Color with_alpha(const Color& c, BYTE a) { return Color(a, c.GetR(), c.GetG(), c.GetB()); }
COLORREF colorref(const Color& c) { return RGB(c.GetR(), c.GetG(), c.GetB()); }
Color mix(const Color& a, const Color& b, float t) {
    auto m = [t](BYTE x, BYTE y) { return static_cast<BYTE>(x + (y - x) * t); };
    return Color(m(a.GetA(), b.GetA()), m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
}
const Color kWhite(255, 255, 255, 255), kBlack(255, 0, 0, 0);

Theme g_t;

bool windows_uses_dark() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v == 0;
}

void pick_theme() {
    const DWORD pref = setting(L"Theme", 0);  // 0 as Windows, 1 light, 2 dark
    const bool dark = pref == 2 || (pref == 0 && windows_uses_dark());
    g_t = dark ? Theme{true, hex(0x1C1C21), hex(0x28282F), hex(0x33333B), hex(0x3B3B44), hex(0xF2F2F7),
                       hex(0x9D9DA6), hex(0x8E80FF), hex(0xFF6B6B), hex(0x6CCB5F)}
               : Theme{false, hex(0xF4F4F7), hex(0xFFFFFF), hex(0xE9E9EF), hex(0xDCDCE3), hex(0x1C1C1E),
                       hex(0x6E6E76), hex(0x6C5CE7), hex(0xC42B1C), hex(0x0F7B0F)};
}

// Dark title bar and, on Windows 11, a caption the colour of the window.
void theme_window(HWND hwnd) {
    const BOOL dark = g_t.dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = colorref(g_t.bg);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
}

// Popup menus follow the theme through uxtheme's unnamed SetPreferredAppMode
// and FlushMenuThemes (ordinals 135 and 136, Windows 10 1903 and later).
void theme_menus() {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto set_mode = reinterpret_cast<SetMode>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
    auto flush = reinterpret_cast<Flush>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
    if (set_mode) set_mode(g_t.dark ? 2 : 3);  // force dark, force light
    if (flush) flush();
}

struct Fonts {
    std::unique_ptr<Gdiplus::Font> title, tagline, body, body_semi, caption, caption_semi, section, tile, hero,
        monogram, monogram_small, glyph12, glyph16, glyph34, glyph48, mono;
} g_f;

// The first of `families` that is installed, else the generic one.
std::unique_ptr<Gdiplus::Font> font_from(std::initializer_list<const wchar_t*> families,
                                         const Gdiplus::FontFamily* generic, float size, int style) {
    for (const wchar_t* name : families) {
        Gdiplus::FontFamily family(name);
        if (family.GetLastStatus() == Gdiplus::Ok && family.IsAvailable()) {
            return std::make_unique<Gdiplus::Font>(&family, px(size), style, Gdiplus::UnitPixel);
        }
    }
    return std::make_unique<Gdiplus::Font>(generic, px(size), style, Gdiplus::UnitPixel);
}

std::unique_ptr<Gdiplus::Font> font(float size, bool semibold = false) {
    if (semibold) {
        Gdiplus::FontFamily family(L"Segoe UI Semibold");
        if (family.GetLastStatus() == Gdiplus::Ok && family.IsAvailable()) {
            return std::make_unique<Gdiplus::Font>(&family, px(size), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        }
    }
    return font_from({L"Segoe UI"}, Gdiplus::FontFamily::GenericSansSerif(), size,
                     semibold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular);
}

std::unique_ptr<Gdiplus::Font> glyph_font(float size) {
    return font_from({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}, Gdiplus::FontFamily::GenericSansSerif(), size,
                     Gdiplus::FontStyleRegular);
}

// Segoe Fluent Icons / MDL2 Assets code points.
constexpr wchar_t kGlyphSettings[] = L"\uE713", kGlyphDownload[] = L"\uE896", kGlyphSearch[] = L"\uE721",
                  kGlyphChevron[] = L"\uE70D", kGlyphCheck[] = L"\uE73E", kGlyphApps[] = L"\uE71D",
                  kGlyphError[] = L"\uE783", kGlyphClock[] = L"\uE823", kGlyphStop[] = L"\uE71A",
                  kGlyphLibrary[] = L"\uE8F1", kGlyphStore[] = L"\uE719";

void make_fonts() {
    g_f.title = font(26, true);
    g_f.tagline = font(13.5f);
    g_f.body = font(14);
    g_f.body_semi = font(14, true);
    g_f.caption = font(12);
    g_f.caption_semi = font(12, true);
    g_f.section = font(16, true);
    g_f.tile = font(13);
    g_f.hero = font(20, true);
    g_f.monogram = font(30, true);
    g_f.monogram_small = font(26, true);
    g_f.glyph12 = glyph_font(12);
    g_f.glyph16 = glyph_font(16);
    g_f.glyph34 = glyph_font(34);
    g_f.glyph48 = glyph_font(48);
    g_f.mono = font_from({L"Cascadia Mono", L"Consolas"}, Gdiplus::FontFamily::GenericMonospace(), 12,
                         Gdiplus::FontStyleRegular);
    if (g_mono_font) DeleteObject(g_mono_font);
    g_mono_font = CreateFontW(-ipx(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    if (g_edit_font) DeleteObject(g_edit_font);
    g_edit_font = CreateFontW(-ipx(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

RectF inset(const RectF& r, float dx, float dy) { return RectF(r.X + dx, r.Y + dy, r.Width - 2 * dx, r.Height - 2 * dy); }
bool contains(const RectF& r, float x, float y) { return x >= r.X && y >= r.Y && x < r.GetRight() && y < r.GetBottom(); }

void rounded(GraphicsPath& p, const RectF& r, float rad) {
    rad = (std::min)(rad, (std::min)(r.Width, r.Height) / 2);
    const float d = rad * 2;
    if (d <= 0) {
        p.AddRectangle(r);
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

void fill_round(Graphics& g, const RectF& r, float rad, const Color& c) {
    GraphicsPath p;
    rounded(p, r, rad);
    Gdiplus::SolidBrush b(c);
    g.FillPath(&b, &p);
}

void stroke_round(Graphics& g, const RectF& r, float rad, const Color& c, float width = 1) {
    GraphicsPath p;
    rounded(p, inset(r, width / 2, width / 2), rad);
    Gdiplus::Pen pen(c, width);
    g.DrawPath(&pen, &p);
}

enum Align { kLeft, kCenter, kRight };

// In Hebrew the layout is mirrored, so the text is drawn without the mirror,
// right to left, in the mirrored place; `ltr` keeps it left to right (logs).
void draw_text(Graphics& g, const std::wstring& s, const Gdiplus::Font* f, const Color& c, const RectF& r,
               Align align = kLeft, bool wrap = false, bool vcenter = true, bool ltr = false) {
    Gdiplus::StringFormat sf;
    sf.SetAlignment(align == kLeft     ? Gdiplus::StringAlignmentNear
                    : align == kCenter ? Gdiplus::StringAlignmentCenter
                                       : Gdiplus::StringAlignmentFar);
    sf.SetLineAlignment(vcenter ? Gdiplus::StringAlignmentCenter : Gdiplus::StringAlignmentNear);
    sf.SetTrimming(wrap ? Gdiplus::StringTrimmingEllipsisWord : Gdiplus::StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(wrap ? Gdiplus::StringFormatFlagsLineLimit : Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush b(c);
    if (!g_rtl) {
        g.DrawString(s.c_str(), static_cast<INT>(s.size()), f, r, &sf, &b);
        return;
    }
    if (!ltr) sf.SetFormatFlags(sf.GetFormatFlags() | Gdiplus::StringFormatFlagsDirectionRightToLeft);
    Gdiplus::Matrix saved;
    g.GetTransform(&saved);
    g.ResetTransform();
    g.DrawString(s.c_str(), static_cast<INT>(s.size()), f, RectF(g_paint_w - r.GetRight(), r.Y, r.Width, r.Height),
                 &sf, &b);
    g.SetTransform(&saved);
}

void draw_image(Graphics& g, Gdiplus::Image* img, const RectF& r) {
    if (!g_rtl) {
        g.DrawImage(img, r);
        return;
    }
    Gdiplus::Matrix saved;
    g.GetTransform(&saved);
    g.ResetTransform();
    g.DrawImage(img, RectF(g_paint_w - r.GetRight(), r.Y, r.Width, r.Height));
    g.SetTransform(&saved);
}

float text_width(Graphics& g, const std::wstring& s, const Gdiplus::Font* f) {
    RectF out;
    g.MeasureString(s.c_str(), static_cast<INT>(s.size()), f, Gdiplus::PointF(0, 0), &out);
    return out.Width;
}

float text_height(Graphics& g, const std::wstring& s, const Gdiplus::Font* f, float width) {
    Gdiplus::StringFormat sf;
    RectF out;
    g.MeasureString(s.c_str(), static_cast<INT>(s.size()), f, RectF(0, 0, width, 100000), &sf, &out);
    return out.Height;
}

void spinner(Graphics& g, const RectF& r, const Color& c, float width) {
    const float a = static_cast<float>(GetTickCount64() % 900) * 360.0f / 900.0f;
    Gdiplus::Pen track(with_alpha(c, 50), width);
    g.DrawEllipse(&track, r);
    Gdiplus::Pen pen(c, width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawArc(&pen, r, a, 100);
}

void draw_monogram(Graphics& g, const RectF& r, const std::wstring& label, const Gdiplus::Font* f) {
    GraphicsPath p;
    rounded(p, r, r.Width * 0.22f);
    Gdiplus::LinearGradientBrush b(r, mix(g_t.accent, kWhite, 0.18f), mix(g_t.accent, kBlack, 0.22f),
                                   Gdiplus::LinearGradientModeForwardDiagonal);
    g.FillPath(&b, &p);
    const std::wstring ch(1, label.empty() ? L'?' : static_cast<wchar_t>(towupper(label[0])));
    draw_text(g, ch, f, kWhite, r, kCenter);
}

void draw_button(Graphics& g, const RectF& r, const std::wstring& text, bool hot, bool primary, bool destructive) {
    const Color fill = primary ? (hot ? mix(g_t.accent, g_t.dark ? kWhite : kBlack, 0.12f) : g_t.accent)
                               : (hot ? g_t.hover : mix(g_t.card, g_t.hover, 0.55f));
    fill_round(g, r, px(6), fill);
    if (!primary) stroke_round(g, r, px(6), g_t.border);
    draw_text(g, text, primary ? g_f.body_semi.get() : g_f.body.get(),
              primary ? kWhite : destructive ? g_t.danger : g_t.text, r, kCenter);
}

// Renders a buffered WM_PAINT through `draw(graphics, width, height)`.
template <class Draw>
void buffered_paint(HWND hwnd, Draw draw) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, (std::max)(1L, rc.right), (std::max)(1L, rc.bottom));
    HGDIOBJ old = SelectObject(mem, bmp);
    SetLayout(mem, 0);
    g_paint_w = static_cast<float>(rc.right);
    {
        Graphics g(mem);
        if (g_rtl) {  // mirror the layout; draw_text and draw_image put text and pictures back the right way round
            Gdiplus::Matrix mirror(-1, 0, 0, 1, g_paint_w, 0);
            g.SetTransform(&mirror);
        }
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        draw(g, static_cast<float>(rc.right), static_cast<float>(rc.bottom));
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY | (g_rtl ? NOMIRRORBITMAP : 0));
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// State

enum StatusKind { kHint, kInfo, kSuccess, kError };

constexpr int kAddTile = -2;

struct Tile {
    int app;           // index into g_apps, -1 for an app still being installed, kAddTile for "Add app"
    std::wstring job;  // that app's name
};

std::vector<App> g_apps;
std::vector<Tile> g_tiles;                 // what the grid shows: new installs first, then the (filtered) apps
std::vector<std::wstring> g_pending;       // names queued or being installed, in order
std::wstring g_current;                    // the one being installed now
int g_batch_total = 0, g_batch_done = 0;   // installs since the queue was last empty
std::set<std::wstring> g_running;          // apps started from this window that are still running
std::set<std::wstring> g_stopping;         // running apps the user stopped: their exit is not an error
struct Job {
    std::wstring name;
    enum State { kWaiting, kInstalling, kDone, kFailed } state = kWaiting;
    ULONGLONG start = 0, end = 0;
};
std::vector<Job> g_jobs;                   // this batch of installs, for the install window
std::map<std::wstring, std::wstring> g_job_labels;  // installs that are not in the library yet: their names to show
HWND g_install_wnd, g_log_wnd, g_log_edit;
std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> g_icons;  // rounded, at the grid's size; null: no icon
std::wstring g_selected;
std::wstring g_status;
StatusKind g_status_kind = kHint;
float g_scroll = 0;
bool g_drag_over = false, g_tracking = false;

// The library and the store share the window's search box and scrolling;
// the view not shown keeps its own query and scroll position here.
enum View { kLibraryView, kStoreView } g_view = kLibraryView;
std::wstring g_other_query;
float g_other_scroll = 0;

struct StoreState {
    std::vector<StoreApp> apps;  // the catalog, the most recently updated first
    std::vector<int> shown;      // what the grid shows: the search and the category applied
    std::wstring category;       // empty: every category
    bool started = false, loading = false, failed = false;
    uint64_t got_kb = 0;
    std::map<std::string, std::pair<std::wstring, long long>> installed;  // package: library name, versionCode
    std::map<std::string, int> downloads;                                  // package: percent, -1 waiting
    std::map<std::string, std::unique_ptr<Gdiplus::Bitmap>> icons;         // rounded; null: none
    std::set<std::string> icon_wait;                                       // being fetched
} g_store;

enum HitKind { kHitNone, kHitGear, kHitDrop, kHitTile, kHitThumb, kHitTrack, kHitStop, kHitLogs, kHitTab, kHitCategory,
               kHitCard, kHitCardButton, kHitRetry };
struct Hit {
    HitKind kind = kHitNone;
    int tile = -1;
    bool operator==(const Hit& o) const { return kind == o.kind && tile == o.tile; }
};
Hit g_hot;
bool g_thumb_drag = false;
float g_thumb_y0 = 0, g_thumb_scroll0 = 0;

struct Layout {
    RectF gear, tabs, drop, section, search, category, grid, footer, logs, stop, retry;
    int cols = 1;
    float tile_w = 0, tile_h = 0, gap = 0, content_h = 0, max_scroll = 0;  // the library's tiles or the store's cards
} g_l;

struct Sheet;
Sheet* g_sheet = nullptr;
HWND sheet_hwnd();
HWND owner() { return sheet_hwnd() ? sheet_hwnd() : g_main; }
void show_logs(const fs::path& log);
void show_install_window();

bool is_pending(const std::wstring& name) {
    return std::find(g_pending.begin(), g_pending.end(), name) != g_pending.end();
}

const App* find_app(const std::wstring& name) {
    for (const App& a : g_apps) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

// The picture in `png` at s x s, with the icons' rounded corners; null if there is none.
std::unique_ptr<Gdiplus::Bitmap> rounded_icon(const fs::path& png, int s) {
    std::unique_ptr<Gdiplus::Bitmap> out;
    std::error_code ec;
    if (fs::exists(png, ec)) {
        Gdiplus::Bitmap src(png.c_str());
        if (src.GetLastStatus() == Gdiplus::Ok && src.GetWidth() > 0) {
            Gdiplus::Bitmap scaled(s, s, PixelFormat32bppPARGB);
            {
                Graphics g(&scaled);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                g.DrawImage(&src, 0, 0, s, s);
            }
            out = std::make_unique<Gdiplus::Bitmap>(s, s, PixelFormat32bppPARGB);
            Graphics g(out.get());
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            Gdiplus::TextureBrush brush(&scaled);
            GraphicsPath p;
            rounded(p, RectF(0, 0, static_cast<float>(s), static_cast<float>(s)), s * 0.22f);
            g.FillPath(&brush, &p);
        }
    }
    return out;
}

Gdiplus::Bitmap* icon_for(const App& app) {
    auto it = g_icons.find(app.name);
    if (it != g_icons.end()) return it->second.get();
    std::unique_ptr<Gdiplus::Bitmap> out = rounded_icon(app_dir(app.name) / L"icon.png", ipx(72));
    Gdiplus::Bitmap* raw = out.get();
    g_icons[app.name] = std::move(out);
    return raw;
}

// An icon with its shadow; the monogram stands in when there is no picture.
void draw_icon(Graphics& g, Gdiplus::Bitmap* bmp, const std::wstring& label, const RectF& r) {
    fill_round(g, RectF(r.X, r.Y + px(2), r.Width, r.Height), r.Width * 0.22f, Color(g_t.dark ? 90 : 30, 0, 0, 0));
    if (bmp) {
        draw_image(g, bmp, r);
    } else {
        draw_monogram(g, r, label,
                      r.Width >= px(72)   ? g_f.monogram.get()
                      : r.Width >= px(56) ? g_f.monogram_small.get()
                                          : g_f.section.get());
    }
}

void draw_app_icon(Graphics& g, const App* app, const std::wstring& label, const RectF& r) {
    draw_icon(g, app ? icon_for(*app) : nullptr, label, r);
}

// The name an install shows before the app is in the library: the store's
// name for it, else the library name.
std::wstring job_label(const std::wstring& name) {
    if (const App* a = find_app(name)) return a->label;
    const auto it = g_job_labels.find(name);
    return it == g_job_labels.end() ? name : it->second;
}

void set_status(const std::wstring& text, StatusKind kind) {
    g_status = text;
    g_status_kind = kind;
    InvalidateRect(g_main, nullptr, FALSE);
}

// A store app's name and summary in Hebrew when F-Droid has them.
const std::wstring& store_name(const StoreApp& a) { return g_rtl && !a.name_he.empty() ? a.name_he : a.name; }
const std::wstring& store_summary(const StoreApp& a) {
    return g_rtl && !a.summary_he.empty() ? a.summary_he : a.summary;
}

// The icon kept from the store, fetched the first time a card asks for it; null meanwhile.
Gdiplus::Bitmap* store_icon(const StoreApp& a) {
    const auto it = g_store.icons.find(a.pkg);
    if (it != g_store.icons.end()) return it->second.get();
    if (g_store.icon_wait.count(a.pkg)) return nullptr;
    std::error_code ec;
    if (a.icon.empty() || fs::exists(store_icon_file(a.pkg), ec)) {
        Gdiplus::Bitmap* raw = (g_store.icons[a.pkg] = rounded_icon(store_icon_file(a.pkg), ipx(56))).get();
        return raw;
    }
    g_store.icon_wait.insert(a.pkg);
    request_icon(a.pkg, a.icon);
    return nullptr;
}

std::vector<std::string> package_of(const std::wstring& name);

// Which packages the library has, and at which version, for the store's buttons.
void scan_installed() {
    g_store.installed.clear();
    for (const App& app : g_apps) {
        const std::vector<std::string> p = package_of(app.name);
        if (p.empty() || p[0].empty()) continue;
        g_store.installed[p[0]] = {app.name, p.size() > 1 ? std::atoll(p[1].c_str()) : 0};
    }
}

enum Offer { kOfferInstall, kOfferUpdate, kOfferOpen, kOfferDownloading, kOfferInstalling };

// What the store offers for an app: to install it, update it (the library's
// version is older; unknown counts as current) or open it, or its progress.
Offer offer_for(const StoreApp& a, int* percent = nullptr) {
    const auto dl = g_store.downloads.find(a.pkg);
    if (dl != g_store.downloads.end()) {
        if (percent) *percent = dl->second;
        return kOfferDownloading;
    }
    const auto it = g_store.installed.find(a.pkg);
    const std::wstring name = it != g_store.installed.end() ? it->second.first : file_name_safe(utf8_to_wide(a.pkg));
    if (is_pending(name)) return kOfferInstalling;
    if (it == g_store.installed.end()) return kOfferInstall;
    return it->second.second > 0 && a.code > it->second.second ? kOfferUpdate : kOfferOpen;
}

// ---------------------------------------------------------------------------
// Layout

RectF tile_rect(int i) {
    const int col = i % g_l.cols, row = i / g_l.cols;
    const float x0 = g_l.grid.X + (g_view == kStoreView ? px(8) : 0);
    return RectF(x0 + col * (g_l.tile_w + g_l.gap), g_l.grid.Y + px(4) + row * (g_l.tile_h + g_l.gap) - g_scroll,
                 g_l.tile_w, g_l.tile_h);
}

// The store card's button, in its bottom corner.
RectF card_button(const RectF& card) {
    return RectF(card.GetRight() - px(14) - px(104), card.GetBottom() - px(12) - px(30), px(104), px(30));
}

RectF thumb_rect() {
    const RectF& gr = g_l.grid;
    const float h = (std::max)(px(36), gr.Height * gr.Height / g_l.content_h);
    const float y = gr.Y + (gr.Height - h) * (g_l.max_scroll > 0 ? g_scroll / g_l.max_scroll : 0);
    return RectF(gr.GetRight() - px(8), y, px(6), h);
}

void clamp_scroll() { g_scroll = (std::max)(0.0f, (std::min)(g_scroll, g_l.max_scroll)); }

void compute_layout() {
    RECT rc;
    GetClientRect(g_main, &rc);
    const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom), m = px(28);
    const bool store = g_view == kStoreView;
    g_l.gear = RectF(W - m - px(36), px(24), px(36), px(36));
    g_l.tabs = RectF(m, px(26), px(216), px(32));
    g_l.drop = RectF(m, px(100), W - 2 * m, px(124));
    g_l.section = RectF(m, store ? px(96) : g_l.drop.GetBottom() + px(18), W - 2 * m, px(36));
    const float search_w = (std::min)(px(260), g_l.section.Width * 0.4f);
    g_l.search = RectF(g_l.section.GetRight() - search_w, g_l.section.Y + px(2), search_w, px(32));
    const float category_w = (std::min)(px(190), g_l.section.Width * 0.28f);
    g_l.category = RectF(g_l.search.X - px(10) - category_w, g_l.search.Y, category_w, px(32));
    g_l.footer = RectF(0, H - px(34), W, px(34));
    g_l.logs = RectF(W - px(10) - px(76), g_l.footer.Y + px(5), px(76), px(24));
    g_l.stop = RectF(g_l.logs.X - px(6) - px(72), g_l.logs.Y, px(72), px(24));
    const float top = g_l.section.GetBottom() + px(8);
    g_l.grid = RectF(m - px(8), top, W - 2 * m + px(16), (std::max)(0.0f, g_l.footer.Y - top));
    g_l.retry = RectF(g_l.grid.X + g_l.grid.Width / 2 - px(60),
                      g_l.grid.Y + (std::min)(g_l.grid.Height / 2, px(110)) + px(66), px(120), px(34));
    size_t items = g_tiles.size();
    if (store) {  // cards, as many across as fit at 300 px or more
        g_l.gap = px(10);
        g_l.tile_h = px(112);
        g_l.cols = (std::max)(1, static_cast<int>((g_l.grid.Width - px(26) + g_l.gap) / (px(300) + g_l.gap)));
        g_l.tile_w = (g_l.grid.Width - px(26) - (g_l.cols - 1) * g_l.gap) / g_l.cols;
        items = g_store.shown.size();
    } else {
        g_l.tile_w = px(124);
        g_l.tile_h = px(138);
        g_l.gap = px(6);
        g_l.cols = (std::max)(1, static_cast<int>((g_l.grid.Width - px(10) + g_l.gap) / (g_l.tile_w + g_l.gap)));
    }
    const int rows = (static_cast<int>(items) + g_l.cols - 1) / g_l.cols;
    g_l.content_h = rows ? rows * (g_l.tile_h + g_l.gap) - g_l.gap + px(12) : 0;
    g_l.max_scroll = (std::max)(0.0f, g_l.content_h - g_l.grid.Height);
    clamp_scroll();
    const int edit_h = ipx(20);
    MoveWindow(g_search, static_cast<int>(g_l.search.X + px(34)),
               static_cast<int>(g_l.search.Y + (g_l.search.Height - edit_h) / 2),
               static_cast<int>(g_l.search.Width - px(44)), edit_h, TRUE);
}

std::wstring query(View view) {
    if (view != g_view) return g_other_query;
    wchar_t buf[256] = {};
    GetWindowTextW(g_search, buf, 256);
    return buf;
}

// The store's cards for the search and category: by name first (the exact
// name, those starting with the words, then those containing them), then by summary or
// package; with no search, the most recently updated first.
void rebuild_store() {
    const std::wstring q = lower(query(kStoreView));
    std::vector<std::pair<int, int>> ranked;  // rank, index
    for (size_t i = 0; i < g_store.apps.size(); ++i) {
        const StoreApp& a = g_store.apps[i];
        if (!g_store.category.empty() &&
            std::find(a.categories.begin(), a.categories.end(), g_store.category) == a.categories.end()) {
            continue;
        }
        int rank = 0;
        if (!q.empty()) {
            const std::wstring name = lower(store_name(a));
            const size_t at = name.find(q);
            if (name == q) rank = 0;
            else if (at == 0) rank = 1;
            else if (at != std::wstring::npos || lower(a.name).find(q) != std::wstring::npos) rank = 2;
            else if (lower(store_summary(a)).find(q) != std::wstring::npos ||
                     lower(utf8_to_wide(a.pkg)).find(q) != std::wstring::npos) rank = 3;
            else continue;
        }
        ranked.emplace_back(rank, static_cast<int>(i));
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    g_store.shown.clear();
    for (const auto& r : ranked) g_store.shown.push_back(r.second);
}

void rebuild_tiles() {
    const std::wstring q = lower(query(kLibraryView));
    auto matches = [&](const std::wstring& a, const std::wstring& b) {
        return q.empty() || lower(a).find(q) != std::wstring::npos || lower(b).find(q) != std::wstring::npos;
    };
    g_tiles.clear();
    std::set<std::wstring> shown;
    for (const std::wstring& name : g_pending) {
        if (!find_app(name) && shown.insert(name).second && matches(name, name)) g_tiles.push_back({-1, name});
    }
    for (size_t i = 0; i < g_apps.size(); ++i) {
        if (matches(g_apps[i].label, g_apps[i].name)) g_tiles.push_back({static_cast<int>(i), L""});
    }
    if (q.empty() && !g_apps.empty()) g_tiles.push_back({kAddTile, L""});
    rebuild_store();
    compute_layout();
    InvalidateRect(g_main, nullptr, FALSE);
}

void refresh() {
    g_apps = library();
    scan_installed();
    g_icons.clear();
    if (!find_app(g_selected)) g_selected.clear();
    rebuild_tiles();
}

int tile_of(const std::wstring& name) {
    for (size_t i = 0; i < g_tiles.size(); ++i) {
        if (g_tiles[i].app >= 0 && g_apps[static_cast<size_t>(g_tiles[i].app)].name == name) return static_cast<int>(i);
    }
    return -1;
}

void ensure_visible(int tile) {
    if (tile < 0) return;
    const RectF r = tile_rect(tile);
    if (r.Y < g_l.grid.Y) g_scroll -= g_l.grid.Y - r.Y + px(4);
    if (r.GetBottom() > g_l.grid.GetBottom()) g_scroll += r.GetBottom() - g_l.grid.GetBottom() + px(4);
    clamp_scroll();
}

void select_app(const std::wstring& name) {
    g_selected = name;
    if (g_view == kLibraryView) ensure_visible(tile_of(name));
    InvalidateRect(g_main, nullptr, FALSE);
}

const App* selected() { return g_selected.empty() ? nullptr : find_app(g_selected); }

Hit hit_test(float x, float y) {
    const bool store = g_view == kStoreView;
    if (contains(g_l.gear, x, y)) return {kHitGear, -1};
    if (contains(g_l.tabs, x, y)) return {kHitTab, x < g_l.tabs.X + g_l.tabs.Width / 2 ? 0 : 1};
    if (!store && contains(g_l.drop, x, y)) return {kHitDrop, -1};
    if (store && !g_store.apps.empty() && contains(g_l.category, x, y)) return {kHitCategory, -1};
    if (contains(g_l.logs, x, y)) return {kHitLogs, -1};
    if (!g_running.empty() && contains(g_l.stop, x, y)) return {kHitStop, -1};
    if (!contains(g_l.grid, x, y)) return {};
    if (g_l.max_scroll > 0 && x >= g_l.grid.GetRight() - px(12)) {
        return {contains(inset(thumb_rect(), -px(4), 0), x, y) ? kHitThumb : kHitTrack, -1};
    }
    if (store) {
        if (g_store.apps.empty() && g_store.failed && contains(g_l.retry, x, y)) return {kHitRetry, -1};
        const int col = static_cast<int>((x - g_l.grid.X - px(8)) / (g_l.tile_w + g_l.gap));
        const int row = static_cast<int>((y - g_l.grid.Y - px(4) + g_scroll) / (g_l.tile_h + g_l.gap));
        const int i = row * g_l.cols + col;
        if (col < 0 || col >= g_l.cols || row < 0 || i >= static_cast<int>(g_store.shown.size())) return {};
        const RectF r = tile_rect(i);
        if (!contains(r, x, y)) return {};
        return {contains(card_button(r), x, y) ? kHitCardButton : kHitCard, i};
    }
    for (size_t i = 0; i < g_tiles.size(); ++i) {
        if (contains(tile_rect(static_cast<int>(i)), x, y)) return {kHitTile, static_cast<int>(i)};
    }
    return {};
}

// ---------------------------------------------------------------------------
// Painting the library

// Library | Store, a segmented control at the start of the header.
void paint_tabs(Graphics& g) {
    const RectF& r = g_l.tabs;
    fill_round(g, r, px(9), g_t.dark ? mix(g_t.bg, g_t.card, 0.7f) : mix(g_t.bg, g_t.border, 0.55f));
    const float seg_w = (r.Width - px(6)) / 2;
    const std::pair<const wchar_t*, const wchar_t*> tabs[] = {{kGlyphLibrary, L"Library"}, {kGlyphStore, L"Store"}};
    for (int i = 0; i < 2; ++i) {
        const RectF seg(r.X + px(3) + i * seg_w, r.Y + px(3), seg_w, r.Height - px(6));
        const bool on = (i == 1) == (g_view == kStoreView);
        const bool hot = g_hot.kind == kHitTab && g_hot.tile == i;
        if (on) {
            fill_round(g, seg, px(7), g_t.dark ? g_t.hover : kWhite);
            if (!g_t.dark) stroke_round(g, seg, px(7), with_alpha(g_t.border, 200));
        } else if (hot) {
            fill_round(g, seg, px(7), with_alpha(g_t.hover, g_t.dark ? 140 : 200));
        }
        const std::wstring text = tr(tabs[i].second);
        const Gdiplus::Font* f = on ? g_f.body_semi.get() : g_f.body.get();
        const float tw = text_width(g, text, f);
        const float x = seg.X + (seg.Width - tw - px(22)) / 2;
        draw_text(g, tabs[i].first, g_f.glyph12.get(), on ? g_t.accent : g_t.text2, RectF(x, seg.Y, px(16), seg.Height),
                  kCenter);
        draw_text(g, text, f, on || hot ? g_t.text : g_t.text2, RectF(x + px(22), seg.Y, tw + px(4), seg.Height));
    }
}

void paint_header(Graphics& g, float W) {
    draw_text(g, L"Win.apk", g_f.title.get(), g_t.text, RectF(0, px(20), W, px(38)), kCenter);
    if (g_view == kLibraryView) {
        draw_text(g, tr(L"Run Android apps natively on Windows"), g_f.tagline.get(), g_t.text2,
                  RectF(0, px(58), W, px(22)), kCenter);
    }
    paint_tabs(g);
    const bool hot = g_hot.kind == kHitGear;
    if (hot) fill_round(g, g_l.gear, px(8), g_t.hover);
    draw_text(g, kGlyphSettings, g_f.glyph16.get(), hot ? g_t.text : g_t.text2, g_l.gear, kCenter);
}

void paint_drop_zone(Graphics& g) {
    const RectF& r = g_l.drop;
    const bool hot = g_hot.kind == kHitDrop;
    const bool busy = !g_pending.empty();
    Color fill = g_t.card;
    if (g_drag_over) fill = mix(g_t.card, g_t.accent, g_t.dark ? 0.22f : 0.12f);
    else if (hot) fill = mix(g_t.card, g_t.accent, g_t.dark ? 0.08f : 0.04f);
    fill_round(g, r, px(14), fill);
    GraphicsPath p;
    rounded(p, inset(r, px(1), px(1)), px(13));
    Gdiplus::Pen pen(g_drag_over || hot ? g_t.accent : with_alpha(g_t.text2, 120), px(1.5f));
    const Gdiplus::REAL dashes[] = {4, 3};
    pen.SetDashPattern(dashes, 2);
    g.DrawPath(&pen, &p);

    const Color ink = g_drag_over ? g_t.accent : g_t.text;
    const float cy = r.Y + r.Height / 2;
    draw_text(g, kGlyphDownload, g_f.glyph34.get(), busy || g_drag_over ? g_t.accent : with_alpha(ink, 220),
              RectF(r.X, cy - px(50), r.Width, px(44)), kCenter);
    std::wstring title, sub;
    if (g_drag_over) {
        title = tr(L"Drop to install");
        sub = tr(L"Win.apk keeps its own copy, so the download can go");
    } else if (busy) {
        title = g_batch_total > 1 ? fmt(L"Installing {0} of {1}…",
                                        {std::to_wstring((std::min)(g_batch_done + 1, g_batch_total)),
                                         std::to_wstring(g_batch_total)})
                                  : std::wstring(tr(L"Installing…"));
        sub = g_current.empty() ? tr(L"Unpacking manifest, icon, and assets")
                                : fmt(L"{0}  ·  the first install of an app takes up to a minute",
                                      {job_label(g_current)});
    } else {
        title = tr(L"Drop an APK here to install");
        sub = tr(L"or click to choose a file  ·  .apk .apkm .xapk .apks");
    }
    draw_text(g, title, g_f.body_semi.get(), ink, RectF(r.X + px(16), cy, r.Width - px(32), px(22)), kCenter);
    draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(r.X + px(16), cy + px(22), r.Width - px(32), px(18)),
              kCenter);
    if (busy) {  // indeterminate bar along the bottom edge
        const RectF track(r.X + r.Width * 0.3f, r.GetBottom() - px(14), r.Width * 0.4f, px(4));
        fill_round(g, track, px(2), with_alpha(g_t.accent, 40));
        const float t = static_cast<float>(GetTickCount64() % 1400) / 1400.0f;
        const float seg = track.Width * 0.35f;
        float x0 = track.X + (track.Width + seg) * t - seg, x1 = x0 + seg;
        x0 = (std::max)(x0, track.X);
        x1 = (std::min)(x1, track.GetRight());
        if (x1 > x0) fill_round(g, RectF(x0, track.Y, x1 - x0, track.Height), px(2), g_t.accent);
    }
}

// The section's title and a count beside it; returns where the count ends.
float paint_section_title(Graphics& g, const std::wstring& title, size_t n) {
    const RectF& r = g_l.section;
    const float tw = text_width(g, title, g_f.section.get());
    draw_text(g, title, g_f.section.get(), g_t.text, RectF(r.X, r.Y, tw + px(4), r.Height));
    const std::wstring count = std::to_wstring(n);
    const float cw = (std::max)(px(24), text_width(g, count, g_f.caption_semi.get()) + px(12));
    const RectF pill(r.X + tw + px(8), r.Y + (r.Height - px(20)) / 2, cw, px(20));
    fill_round(g, pill, px(10), g_t.dark ? g_t.hover : mix(g_t.bg, g_t.border, 0.6f));
    draw_text(g, count, g_f.caption_semi.get(), g_t.text2, pill, kCenter);
    return pill.GetRight();
}

void paint_search_box(Graphics& g) {
    const bool focused = GetFocus() == g_search;
    fill_round(g, g_l.search, px(8), g_t.card);
    stroke_round(g, g_l.search, px(8), focused ? g_t.accent : g_t.border, focused ? px(1.5f) : 1);
    draw_text(g, kGlyphSearch, g_f.glyph12.get(), g_t.text2,
              RectF(g_l.search.X + px(10), g_l.search.Y, px(16), g_l.search.Height), kCenter);
}

void paint_section(Graphics& g) {
    paint_section_title(g, tr(L"Installed apps"), g_apps.size());
    paint_search_box(g);
}

void paint_scrollbar(Graphics& g, bool nearby) {
    if (g_l.max_scroll <= 0) return;
    const bool active = g_thumb_drag || g_hot.kind == kHitThumb || g_hot.kind == kHitTrack;
    fill_round(g, thumb_rect(), px(3), with_alpha(g_t.text2, active ? 200 : nearby ? 120 : 70));
}

void paint_tile(Graphics& g, int i) {
    const RectF r = tile_rect(i);
    if (r.GetBottom() < g_l.grid.Y || r.Y > g_l.grid.GetBottom()) return;
    const Tile& t = g_tiles[static_cast<size_t>(i)];
    const bool hot_tile = g_hot.kind == kHitTile && g_hot.tile == i;
    if (t.app == kAddTile) {
        if (hot_tile) fill_round(g, r, px(12), g_t.hover);
        const float s = px(56);
        const RectF box(r.X + (r.Width - s) / 2, r.Y + px(22), s, s);
        GraphicsPath p;
        rounded(p, box, px(14));
        Gdiplus::Pen dash(hot_tile ? g_t.accent : with_alpha(g_t.text2, 140), px(1.5f));
        const Gdiplus::REAL dashes[] = {3, 3};
        dash.SetDashPattern(dashes, 2);
        g.DrawPath(&dash, &p);
        Gdiplus::Pen plus(hot_tile ? g_t.accent : g_t.text2, px(2));
        plus.SetStartCap(Gdiplus::LineCapRound);
        plus.SetEndCap(Gdiplus::LineCapRound);
        const float cx = box.X + s / 2, cy = box.Y + s / 2, a = px(10);
        g.DrawLine(&plus, cx - a, cy, cx + a, cy);
        g.DrawLine(&plus, cx, cy - a, cx, cy + a);
        draw_text(g, tr(L"Add app"), g_f.tile.get(), g_t.text2,
                  RectF(r.X + px(6), box.GetBottom() + px(17), r.Width - px(12), px(20)), kCenter);
        return;
    }
    const App* app = t.app >= 0 ? &g_apps[static_cast<size_t>(t.app)] : nullptr;
    const bool sel = app && app->name == g_selected;
    const bool hot = g_hot.kind == kHitTile && g_hot.tile == i;
    if (sel) {
        fill_round(g, r, px(12), with_alpha(g_t.accent, g_t.dark ? 64 : 34));
        stroke_round(g, r, px(12), with_alpha(g_t.accent, 150));
    } else if (hot) {
        fill_round(g, r, px(12), g_t.hover);
    }
    const std::wstring label = app ? app->label : job_label(t.job);
    const float s = px(72);
    const RectF ir(r.X + (r.Width - s) / 2, r.Y + px(14), s, s);
    draw_app_icon(g, app, label, ir);
    if (!app || is_pending(app->name)) {
        fill_round(g, ir, s * 0.22f, Color(130, 0, 0, 0));
        spinner(g, inset(ir, s * 0.3f, s * 0.3f), kWhite, px(3));
    }
    draw_text(g, label, g_f.tile.get(), app ? g_t.text : g_t.text2,
              RectF(r.X + px(6), ir.GetBottom() + px(9), r.Width - px(12), px(36)), kCenter, true, false);
    if (app && g_running.count(app->name)) {
        Gdiplus::SolidBrush dot(g_t.accent);
        g.FillEllipse(&dot, r.X + r.Width / 2 - px(3), r.GetBottom() - px(9), px(6), px(6));
    }
}

void paint_grid(Graphics& g) {
    const RectF& gr = g_l.grid;
    if (g_tiles.empty()) {
        wchar_t q[256] = {};
        GetWindowTextW(g_search, q, 256);
        const float cy = gr.Y + (std::min)(gr.Height / 2, px(110));
        std::wstring title, sub;
        if (q[0]) {
            title = fmt(L"No apps match “{0}”", {q});
            sub = tr(L"Press Esc in the search box to show every app.");
        } else {
            title = tr(L"No apps installed yet.");
            sub = tr(L"Drop an APK above, or get apps from the Store, then open them from here.");
        }
        draw_text(g, q[0] ? kGlyphSearch : kGlyphApps, g_f.glyph48.get(), with_alpha(g_t.text2, 150),
                  RectF(gr.X, cy - px(70), gr.Width, px(60)), kCenter);
        draw_text(g, title, g_f.body_semi.get(), g_t.text, RectF(gr.X, cy, gr.Width, px(24)), kCenter);
        draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(gr.X + px(24), cy + px(24), gr.Width - px(48), px(36)),
                  kCenter, true, false);
        return;
    }
    g.SetClip(gr);
    for (size_t i = 0; i < g_tiles.size(); ++i) paint_tile(g, static_cast<int>(i));
    g.ResetClip();
    if (g_scroll > 0) {
        Gdiplus::Pen line(g_t.border, 1);
        g.DrawLine(&line, gr.X + px(8), gr.Y, gr.GetRight() - px(8), gr.Y);
    }
    paint_scrollbar(g, g_hot.kind == kHitTile);
}

// The store's heading: F-Droid and the count, the catalog's state, the category and the search.
void paint_store_section(Graphics& g) {
    float x = paint_section_title(g, L"F-Droid", g_store.shown.size()) + px(14);
    const RectF& r = g_l.section;
    if (g_store.loading && !g_store.apps.empty()) {
        spinner(g, RectF(x, r.Y + r.Height / 2 - px(6), px(12), px(12)), g_t.accent, px(2));
        draw_text(g, tr(L"Checking F-Droid for new apps…"), g_f.caption.get(), g_t.text2,
                  RectF(x + px(20), r.Y, (std::max)(0.0f, g_l.category.X - x - px(28)), r.Height));
    }
    if (!g_store.apps.empty()) {
        const RectF& c = g_l.category;
        const bool hot = g_hot.kind == kHitCategory;
        fill_round(g, c, px(8), hot ? g_t.hover : g_t.card);
        stroke_round(g, c, px(8), g_store.category.empty() ? g_t.border : g_t.accent);
        const std::wstring text = g_store.category.empty() ? tr(L"All categories") : g_store.category;
        draw_text(g, text, g_f.body.get(), g_store.category.empty() ? g_t.text2 : g_t.text,
                  RectF(c.X + px(12), c.Y, c.Width - px(40), c.Height));
        draw_text(g, kGlyphChevron, g_f.glyph12.get(), g_t.text2, RectF(c.GetRight() - px(30), c.Y, px(22), c.Height),
                  kCenter);
    }
    paint_search_box(g);
}

void paint_card(Graphics& g, int i) {
    const RectF r = tile_rect(i);
    if (r.GetBottom() < g_l.grid.Y || r.Y > g_l.grid.GetBottom()) return;
    const StoreApp& a = g_store.apps[static_cast<size_t>(g_store.shown[static_cast<size_t>(i)])];
    const bool hot = (g_hot.kind == kHitCard || g_hot.kind == kHitCardButton) && g_hot.tile == i;
    fill_round(g, r, px(12), hot ? mix(g_t.card, g_t.hover, 0.5f) : g_t.card);
    stroke_round(g, r, px(12), hot ? mix(g_t.border, g_t.accent, 0.35f) : g_t.border);
    const float s = px(56);
    const RectF ir(r.X + px(14), r.Y + px(14), s, s);
    draw_icon(g, store_icon(a), store_name(a), ir);
    const float tx = ir.GetRight() + px(14), tw = r.GetRight() - px(14) - tx;
    draw_text(g, store_name(a), g_f.body_semi.get(), g_t.text, RectF(tx, r.Y + px(12), tw, px(22)));
    draw_text(g, store_summary(a), g_f.caption.get(), g_t.text2, RectF(tx, r.Y + px(35), tw, px(34)), kLeft, true,
              false);
    const RectF b = card_button(r);
    std::wstring meta = a.version;
    if (a.size > 0) meta += (meta.empty() ? L"" : L"  ·  ") + format_bytes(static_cast<uintmax_t>(a.size));
    draw_text(g, meta, g_f.caption.get(), with_alpha(g_t.text2, 200),
              RectF(r.X + px(14), b.Y, b.X - r.X - px(24), b.Height));
    int percent = 0;
    const bool on_button = g_hot.kind == kHitCardButton && g_hot.tile == i;
    switch (offer_for(a, &percent)) {
        case kOfferInstall: draw_button(g, b, tr(L"Install"), on_button, true, false); break;
        case kOfferUpdate: draw_button(g, b, tr(L"Update"), on_button, true, false); break;
        case kOfferOpen: draw_button(g, b, tr(L"Open"), on_button, false, false); break;
        case kOfferDownloading: {
            fill_round(g, b, px(6), with_alpha(g_t.accent, g_t.dark ? 40 : 24));
            if (percent > 0) {
                const RectF bar(b.X, b.Y, b.Width * static_cast<float>(percent) / 100.0f, b.Height);
                g.SetClip(bar, Gdiplus::CombineModeIntersect);
                fill_round(g, b, px(6), with_alpha(g_t.accent, g_t.dark ? 120 : 80));
                g.SetClip(g_l.grid);
            }
            draw_text(g, percent > 0 ? std::to_wstring(percent) + L"%" : std::wstring(tr(L"Waiting")),
                      g_f.caption_semi.get(), g_t.text, b, kCenter);
            break;
        }
        case kOfferInstalling: {
            spinner(g, RectF(b.X + px(10), b.Y + b.Height / 2 - px(7), px(14), px(14)), g_t.accent, px(2));
            draw_text(g, tr(L"Installing…"), g_f.caption.get(), g_t.text2,
                      RectF(b.X + px(30), b.Y, b.Width - px(30), b.Height));
            break;
        }
    }
}

void paint_store_grid(Graphics& g) {
    const RectF& gr = g_l.grid;
    if (g_store.shown.empty()) {
        const float cy = gr.Y + (std::min)(gr.Height / 2, px(110));
        const std::wstring q = query(kStoreView);
        std::wstring title, sub;
        const wchar_t* glyph = kGlyphStore;
        if (g_store.apps.empty() && (g_store.loading || !g_store.started)) {
            spinner(g, RectF(gr.X + gr.Width / 2 - px(20), cy - px(58), px(40), px(40)), g_t.accent, px(3.5f));
            glyph = nullptr;
            title = tr(L"Getting the app catalog from F-Droid…");
            wchar_t mb[32];
            swprintf(mb, 32, L"%.1f", static_cast<double>(g_store.got_kb) / 1024.0);
            sub = fmt(L"{0} MB so far. This happens once; later the store opens at once.", {mb});
        } else if (g_store.apps.empty()) {
            glyph = kGlyphError;
            title = tr(L"F-Droid could not be reached");
            sub = tr(L"Check the internet connection, then try again.");
            draw_button(g, g_l.retry, tr(L"Try again"), g_hot.kind == kHitRetry, true, false);
        } else {
            glyph = kGlyphSearch;
            title = fmt(L"No apps match “{0}”", {q.empty() ? g_store.category : q});
            sub = tr(L"Try other words, or every category.");
        }
        if (glyph) {
            draw_text(g, glyph, g_f.glyph48.get(), with_alpha(g_t.text2, 150),
                      RectF(gr.X, cy - px(70), gr.Width, px(60)), kCenter);
        }
        draw_text(g, title, g_f.body_semi.get(), g_t.text, RectF(gr.X, cy, gr.Width, px(24)), kCenter);
        draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(gr.X + px(24), cy + px(24), gr.Width - px(48), px(36)),
                  kCenter, true, false);
        return;
    }
    g.SetClip(gr);
    const int first_row = (std::max)(0, static_cast<int>(g_scroll / (g_l.tile_h + g_l.gap)));
    const int last = (std::min)(static_cast<int>(g_store.shown.size()),
                                (first_row + static_cast<int>(gr.Height / (g_l.tile_h + g_l.gap)) + 2) * g_l.cols);
    for (int i = first_row * g_l.cols; i < last; ++i) paint_card(g, i);
    g.ResetClip();
    if (g_scroll > 0) {
        Gdiplus::Pen line(g_t.border, 1);
        g.DrawLine(&line, gr.X + px(8), gr.Y, gr.GetRight() - px(8), gr.Y);
    }
    paint_scrollbar(g, g_hot.kind == kHitCard || g_hot.kind == kHitCardButton);
}

// A small borderless button with a glyph, for the footer.
void draw_chip(Graphics& g, const RectF& r, const wchar_t* glyph, const std::wstring& text, const Color& c, bool hot) {
    if (hot) fill_round(g, r, px(6), g_t.hover);
    const float tw = text_width(g, text, g_f.caption.get());
    const float x = r.X + (r.Width - tw - px(18)) / 2;
    draw_text(g, glyph, g_f.glyph12.get(), c, RectF(x, r.Y, px(14), r.Height), kCenter);
    draw_text(g, text, g_f.caption.get(), c, RectF(x + px(18), r.Y, tw + px(2), r.Height));
}

const App* running_app() {
    if (const App* a = selected(); a && g_running.count(a->name)) return a;
    return g_running.empty() ? nullptr : find_app(*g_running.begin());
}

void paint_footer(Graphics& g, float W) {
    const RectF& r = g_l.footer;
    Gdiplus::SolidBrush bar(g_t.dark ? mix(g_t.bg, kBlack, 0.15f) : mix(g_t.bg, kWhite, 0.5f));
    g.FillRectangle(&bar, r);
    Gdiplus::Pen line(g_t.border, 1);
    g.DrawLine(&line, 0.0f, r.Y, W, r.Y);
    draw_chip(g, g_l.logs, L"\uE8A5", tr(L"Logs"), g_t.text2, g_hot.kind == kHitLogs);
    const App* running = running_app();
    if (running) draw_chip(g, g_l.stop, kGlyphStop, tr(L"Stop"), g_t.danger, g_hot.kind == kHitStop);
    const float right = running ? g_l.stop.X : g_l.logs.X;

    const bool store = g_view == kStoreView;
    const std::wstring hint =
        store            ? tr(L"Free and open-source apps from f-droid.org  ·  Install adds the app to your library")
        : g_apps.empty() ? tr(L"Drop an APK above to install your first app")
                         : tr(L"Double-click to open  ·  right-click for options  ·  drop an APK above to add more");
    const bool hinted = g_status.empty() || g_status_kind == kHint;
    const Color c = hinted || g_status_kind == kInfo ? g_t.text2
                    : g_status_kind == kSuccess       ? g_t.success
                                                      : g_t.danger;
    const std::wstring count = app_count(store ? g_store.shown.size() : g_apps.size());
    const float cw = text_width(g, count, g_f.caption.get()) + px(4);
    float x = px(16);
    std::wstring text = hinted ? hint : g_status;
    if (running && (hinted || g_status_kind == kInfo)) {
        spinner(g, RectF(x, r.Y + r.Height / 2 - px(6), px(12), px(12)), g_t.accent, px(2));
        x += px(20);
        text = g_running.size() > 1 ? fmt(L"{0} apps are running", {std::to_wstring(g_running.size())})
                                    : fmt(L"{0} is running", {running->label});
    } else if (!hinted && g_status_kind != kInfo) {
        Gdiplus::SolidBrush dot(c);
        g.FillEllipse(&dot, x, r.Y + r.Height / 2 - px(3), px(6), px(6));
        x += px(14);
    }
    draw_text(g, text, g_f.caption.get(), running ? g_t.text : c, RectF(x, r.Y, right - x - cw - px(24), r.Height));
    draw_text(g, count, g_f.caption.get(), g_t.text2, RectF(right - px(12) - cw, r.Y, cw, r.Height), kRight);
}

void paint_main(Graphics& g, float W, float) {
    g.Clear(g_t.bg);
    paint_header(g, W);
    if (g_view == kStoreView) {
        paint_store_section(g);
        paint_store_grid(g);
    } else {
        paint_drop_zone(g);
        paint_section(g);
        paint_grid(g);
    }
    paint_footer(g, W);
}

void update_timer() {
    if (!g_pending.empty() || !g_running.empty() || (g_view == kStoreView && g_store.loading)) {
        SetTimer(g_main, kAnimTimer, 16, nullptr);
    }
    else KillTimer(g_main, kAnimTimer);
}

// ---------------------------------------------------------------------------
// Dialogs

int task_dialog(const std::wstring& main, const std::wstring& content, PCWSTR icon,
                std::vector<TASKDIALOG_BUTTON> buttons, bool cancel, const std::wstring& details = {}) {
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof(c);
    c.hwndParent = owner();
    c.hInstance = g_inst;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | (g_rtl ? TDF_RTL_LAYOUT : 0);
    c.pszWindowTitle = L"Win.apk";
    c.pszMainIcon = icon;
    c.pszMainInstruction = main.c_str();
    c.pszContent = content.c_str();
    c.cButtons = static_cast<UINT>(buttons.size());
    c.pButtons = buttons.data();
    c.dwCommonButtons = cancel ? TDCBF_CANCEL_BUTTON : TDCBF_CLOSE_BUTTON;
    if (!details.empty()) {
        c.pszExpandedInformation = details.c_str();
        c.pszExpandedControlText = tr(L"What the log says");
        c.pszCollapsedControlText = c.pszExpandedControlText;
        c.dwFlags |= TDF_EXPANDED_BY_DEFAULT;
    }
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) return 0;
    return pressed;
}

bool confirm(const std::wstring& main, const std::wstring& content, const wchar_t* action) {
    return task_dialog(main, content, TD_WARNING_ICON, {{IDOK, action}}, true) == IDOK;
}

std::string read_tail(const fs::path& p, size_t max_bytes);

// The lines of a log that say why an app failed: its errors, exceptions and the exit. Each line starts
// with a left-to-right mark, so a Hebrew (mirrored) dialog keeps an English log line's own order.
std::wstring log_summary(const fs::path& log, bool* no_native_code) {
    std::vector<std::wstring> hits;
    const std::wstring raw = utf8_to_wide(read_tail(log, 256 * 1024));
    size_t at = 0;
    while (at < raw.size()) {
        size_t end = raw.find(L'\n', at);
        if (end == std::wstring::npos) end = raw.size();
        std::wstring l = raw.substr(at, end - at);
        at = end + 1;
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        if (l.find(L"not shipped in this APK") != std::wstring::npos) *no_native_code = true;
        const bool why = l.rfind(L"[E]", 0) == 0 || l.rfind(L"[F]", 0) == 0 || l.find(L"FATAL") != std::wstring::npos ||
                         l.find(L"Exception:") != std::wstring::npos || l.find(L"Error:") != std::wstring::npos ||
                         l.find(L"System.exit(") != std::wstring::npos;
        if (!why) continue;
        if (l.size() > 220) l = l.substr(0, 220) + L"\x2026";
        hits.push_back(std::move(l));
    }
    std::wstring out;
    for (size_t i = hits.size() > 12 ? hits.size() - 12 : 0; i < hits.size(); ++i) out += L"\x200E" + hits[i] + L"\n";
    if (!out.empty()) out.pop_back();
    return out;
}

// Whether the run got as far as drawing: the runtime logs its first presented frame. An app that exits
// cleanly before that (an APK without its native code, a check that gives up) failed to open all the same.
bool log_shows_window(const fs::path& log) {
    HANDLE f = CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return true;  // nothing to go on: don't call it a failure
    static const std::string kMark = "] first frame";
    std::string buf, chunk(1 << 20, '\0');
    bool found = false;
    DWORD got = 0;
    while (!found && ReadFile(f, &chunk[0], static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
        buf.append(chunk, 0, got);
        found = buf.find(kMark) != std::string::npos ||
                buf.find("native window: first frame") != std::string::npos;
        if (buf.size() > kMark.size() + 64) buf.erase(0, buf.size() - (kMark.size() + 64));  // keep the seam
    }
    CloseHandle(f);
    return found;
}

void failed_with_log(const std::wstring& main, const fs::path& log, bool in_notepad = false) {
    bool no_native_code = false;
    const std::wstring details = log_summary(log, &no_native_code);
    std::wstring content = fmt(L"The log may say why:\n{0}", {log.wstring()});
    if (no_native_code) {
        content = std::wstring(tr(L"This APK has no native code for this app (lib/arm64-v8a). Install the full bundle "
                                  L"(.apkm, .xapk or .apks) that includes the arm64-v8a part.")) +
                  L"\n\n" + content;
    }
    if (task_dialog(main, content, TD_ERROR_ICON, {{100, tr(L"Open the log")}}, false, details) == 100) {
        if (in_notepad) open_log(log);
        else show_logs(log);
    }
}

std::vector<fs::path> pick_files(bool multiple, const wchar_t* title) {
    std::vector<fs::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    const COMDLG_FILTERSPEC types[] = {{tr(L"Android apps"), L"*.apk;*.apkm;*.xapk;*.apks"}, {tr(L"All files"), L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(title);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FILEMUSTEXIST | (multiple ? static_cast<DWORD>(FOS_ALLOWMULTISELECT) : 0u));
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlg->Show(owner())) && SUCCEEDED(dlg->GetResults(&items))) {
        DWORD n = 0;
        items->GetCount(&n);
        for (DWORD i = 0; i < n; ++i) {
            IShellItem* item = nullptr;
            PWSTR path = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out.emplace_back(path);
            }
            CoTaskMemFree(path);
            if (item) item->Release();
        }
        items->Release();
    }
    dlg->Release();
    return out;
}

// ---------------------------------------------------------------------------
// Actions

// Queues an install. The store's (`from_store`) installs a download it then
// removes, and shows its progress on the app's card rather than in the
// install window.
void install(const fs::path& source, bool run_after, std::wstring name = L"", fs::path replaces = {},
             bool from_store = false) {
    if (name.empty()) name = file_name_safe(source.stem().wstring());
    if (g_pending.empty()) {
        g_batch_total = g_batch_done = 0;
        g_jobs.clear();
    }
    ++g_batch_total;
    g_pending.push_back(name);
    g_jobs.push_back({name});
    set_status(fmt(L"Installing {0}…", {job_label(name)}), kInfo);
    CloseHandle(CreateThread(nullptr, 0, install_thread, new InstallJob{source, name, replaces, run_after, from_store},
                             0, nullptr));
    update_timer();
    rebuild_tiles();
    if (!from_store || g_install_wnd) show_install_window();
}

void add_from_dialog() {
    for (const fs::path& p : pick_files(true, tr(L"Install an Android app"))) install(p, false);
}

void launch(const App& app) {
    if (is_pending(app.name)) {
        set_status(fmt(L"{0} is still being installed.", {app.label}), kInfo);
        return;
    }
    if (g_running.count(app.name)) {
        set_status(fmt(L"{0} is already open.", {app.label}), kInfo);
        return;
    }
    g_running.insert(app.name);
    set_status(fmt(L"Opening {0}…", {app.label}), kInfo);
    CloseHandle(CreateThread(nullptr, 0, run_thread, new RunJob{app.name, app.apk}, 0, nullptr));
    update_timer();
}

bool remove_app(const App& app, bool ask) {
    if (ask && !confirm(fmt(L"Remove {0}?", {app.label}),
                        tr(L"Its shortcuts go too. What the app saved stays, so installing it again picks up where it left off."),
                        tr(L"Remove"))) {
        return false;
    }
    const App copy = app;  // `app` may point into g_apps, which refresh() replaces
    std::error_code ec;
    remove_app_shortcuts(copy);
    fs::remove(copy.apk, ec);
    fs::remove(label_file(copy.name), ec);
    fs::remove(package_file(copy.name), ec);
    fs::remove(shortcut_icon(copy.name), ec);
    fs::remove_all(app_dir(copy.name), ec);
    fs::remove(data_dir() / L"android" / L"data" / L"app" / (L"." + copy.name + L".installed"), ec);
    write_registry();
    if (ask) {
        refresh();
        set_status(fmt(L"{0} was removed.", {copy.label}), kInfo);
    }
    return true;
}

void update_from_apk(const App& app) {
    const auto files = pick_files(false, fmt(L"Update {0} from an APK", {app.label}).c_str());
    if (!files.empty()) install(files[0], false, app.name, app.apk);
}

// Fetches the catalog: the one kept from last time at once, F-Droid's when
// that is old or `force` asks for it.
void store_start(bool force) {
    if (g_store.loading) return;
    g_store.started = g_store.loading = true;
    g_store.failed = false;
    g_store.got_kb = 0;
    if (HANDLE t = CreateThread(nullptr, 0, catalog_thread, force ? &g_store : nullptr, 0, nullptr)) CloseHandle(t);
    else g_store.loading = false;
    update_timer();
    InvalidateRect(g_main, nullptr, FALSE);
}

// The card's button: install or update the app (download, check, then the
// library's install), or open it.
void store_get(const StoreApp& a) {
    const Offer offer = offer_for(a);
    if (offer == kOfferOpen) {
        const auto it = g_store.installed.find(a.pkg);
        if (const App* app = it == g_store.installed.end() ? nullptr : find_app(it->second.first)) launch(*app);
        return;
    }
    if (offer != kOfferInstall && offer != kOfferUpdate) return;
    g_store.downloads[a.pkg] = -1;
    CloseHandle(CreateThread(nullptr, 0, download_thread,
                             new Download{a.pkg, a.sha256, std::wstring(kRepo) + utf8_to_wide(a.apk), store_name(a),
                                          downloads_dir() / utf8_to_wide(a.apk)},
                             0, nullptr));
    set_status(fmt(L"Downloading {0}…", {store_name(a)}), kInfo);
    InvalidateRect(g_main, nullptr, FALSE);
}

// A download is in and checked: it is installed as the app the library has
// with that package, if any, else as a new one named after the package.
void store_downloaded(const Download& d, bool ok) {
    g_store.downloads.erase(d.pkg);
    if (!ok) {
        set_status(fmt(L"Downloading {0} failed. Check the internet connection, then try again.", {d.label}), kError);
        return;
    }
    const auto it = g_store.installed.find(d.pkg);
    const App* existing = it == g_store.installed.end() ? nullptr : find_app(it->second.first);
    const std::wstring name = existing ? existing->name : file_name_safe(utf8_to_wide(d.pkg));
    if (!existing) g_job_labels[name] = d.label;
    install(d.file, false, name, existing ? existing->apk : fs::path(), true);
}

// The categories, in a menu of a few columns so that a hundred of them fit.
void choose_category() {
    std::set<std::wstring, bool (*)(const std::wstring&, const std::wstring&)> all(
        [](const std::wstring& a, const std::wstring& b) {
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.c_str(), -1, b.c_str(), -1, nullptr,
                                   nullptr, 0) == CSTR_LESS_THAN;
        });
    for (const StoreApp& a : g_store.apps) all.insert(a.categories.begin(), a.categories.end());
    const std::vector<std::wstring> list(all.begin(), all.end());
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_store.category.empty() ? MF_CHECKED : 0), 1, tr(L"All categories"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const size_t per_column = (std::max)(size_t{20}, (list.size() + 4) / 5);
    for (size_t i = 0; i < list.size(); ++i) {
        const UINT flags = MF_STRING | (list[i] == g_store.category ? MF_CHECKED : 0) |
                           (i > 0 && i % per_column == 0 ? MF_MENUBARBREAK : 0);
        AppendMenuW(menu, flags, i + 2, list[i].c_str());
    }
    POINT pt{static_cast<LONG>(g_l.category.X), static_cast<LONG>(g_l.category.GetBottom() + px(2))};
    ClientToScreen(g_main, &pt);
    const int pick = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_TOPALIGN | menu_flags(true), pt.x, pt.y, 0, g_main,
                                    nullptr);
    DestroyMenu(menu);
    if (pick <= 0) return;
    g_store.category = pick == 1 ? L"" : list[static_cast<size_t>(pick - 2)];
    g_scroll = 0;
    rebuild_tiles();
}

// ---------------------------------------------------------------------------
// Sheets: the settings and Get Info windows, drawn like upstream's grouped forms

struct Row {
    enum Kind { kText, kButton, kToggle, kChoice } kind = kText;
    std::wstring title, detail, value;  // value: the button's text, or the text on the right
    bool on = false, destructive = false, primary = false, check = false;
    std::vector<std::wstring> choices;
    int choice = 0;
    std::function<void(int)> act;  // button: act(0), toggle: act(new state), choice: act(index)
};

struct Section {
    std::wstring title;
    std::vector<Row> rows;
    std::wstring footer;
};

struct Sheet {
    std::wstring title;
    std::wstring hero_app, hero_title, hero_sub;  // Get Info's heading: the app's icon, name and package
    Gdiplus::Bitmap* hero_icon = nullptr;         // a store app's icon, for one not in the library
    std::function<std::vector<Section>()> build;
    std::vector<Section> sections;
    HWND hwnd = nullptr;
    float scroll = 0, content_h = 0, view_h = 0;
    struct Target {
        RectF r;
        int section, row;
    };
    std::vector<Target> targets;
    RectF done;
    int hot = -1;  // index into targets, -2 the Done button
    bool tracking = false;
};

HWND sheet_hwnd() { return g_sheet ? g_sheet->hwnd : nullptr; }

void close_sheet() {
    if (!g_sheet || !g_sheet->hwnd) return;
    EnableWindow(g_main, TRUE);  // before the sheet goes, so Windows activates the library and not another app
    DestroyWindow(g_sheet->hwnd);
}

SizeF control_size(Graphics& g, const Row& row) {
    switch (row.kind) {
        case Row::kButton: return SizeF(text_width(g, row.value, g_f.body_semi.get()) + px(32), px(32));
        case Row::kToggle: return SizeF(px(44), px(22));
        case Row::kChoice: {
            float w = 0;
            for (const auto& c : row.choices) w = (std::max)(w, text_width(g, c, g_f.body.get()));
            return SizeF((std::min)(px(250), w + px(48)), px(32));
        }
        case Row::kText:
            if (row.value.empty()) return SizeF(0, 0);
            return SizeF((std::min)(px(260), text_width(g, row.value, g_f.body.get()) + (row.check ? px(22) : 0)),
                         px(22));
    }
    return SizeF(0, 0);
}

void paint_control(Graphics& g, const Row& row, const RectF& r, bool hot) {
    switch (row.kind) {
        case Row::kButton: draw_button(g, r, row.value, hot, row.primary, row.destructive); break;
        case Row::kToggle: {
            fill_round(g, r, r.Height / 2, row.on ? (hot ? mix(g_t.accent, kWhite, 0.1f) : g_t.accent)
                                                  : (hot ? g_t.hover : g_t.card));
            if (!row.on) stroke_round(g, r, r.Height / 2, g_t.text2);
            const float k = row.on ? r.Height - px(8) : r.Height - px(10);
            const float kx = row.on ? r.GetRight() - k - px(4) : r.X + px(5);
            Gdiplus::SolidBrush knob(row.on ? kWhite : g_t.text2);
            g.FillEllipse(&knob, kx, r.Y + (r.Height - k) / 2, k, k);
            break;
        }
        case Row::kChoice: {
            fill_round(g, r, px(6), hot ? g_t.hover : mix(g_t.card, g_t.hover, 0.55f));
            stroke_round(g, r, px(6), g_t.border);
            const std::wstring& v = row.choices.empty() ? std::wstring() : row.choices[static_cast<size_t>(row.choice)];
            draw_text(g, v, g_f.body.get(), g_t.text, RectF(r.X + px(12), r.Y, r.Width - px(40), r.Height));
            draw_text(g, kGlyphChevron, g_f.glyph12.get(), g_t.text2,
                      RectF(r.GetRight() - px(30), r.Y, px(22), r.Height), kCenter);
            break;
        }
        case Row::kText: {
            float x = r.X;
            if (row.check) {
                draw_text(g, kGlyphCheck, g_f.glyph16.get(), g_t.success, RectF(x, r.Y, px(18), r.Height), kCenter);
                x += px(22);
            }
            draw_text(g, row.value, g_f.body.get(), g_t.text2, RectF(x, r.Y, r.GetRight() - x, r.Height), kRight);
            break;
        }
    }
}

void paint_sheet(Sheet& s, Graphics& g, float W, float H) {
    g.Clear(g_t.bg);
    s.targets.clear();
    const float m = px(24), pad = px(16), foot = px(64), cw = W - 2 * m;
    s.view_h = H - foot;
    g.SetClip(RectF(0, 0, W, s.view_h));
    float y = px(20) - s.scroll;
    if (!s.hero_title.empty()) {
        const RectF ir(m, y, px(64), px(64));
        if (s.hero_icon || s.hero_app.empty()) draw_icon(g, s.hero_icon, s.hero_title, ir);
        else draw_app_icon(g, find_app(s.hero_app), s.hero_title, ir);
        draw_text(g, s.hero_title, g_f.hero.get(), g_t.text, RectF(m + px(80), y + px(6), cw - px(80), px(30)));
        draw_text(g, s.hero_sub, g_f.caption.get(), g_t.text2, RectF(m + px(80), y + px(36), cw - px(80), px(20)));
        y += px(88);
    }
    for (size_t si = 0; si < s.sections.size(); ++si) {
        const Section& sec = s.sections[si];
        if (!sec.title.empty()) {
            draw_text(g, sec.title, g_f.caption_semi.get(), g_t.text2, RectF(m + px(4), y, cw, px(20)));
            y += px(24);
        }
        std::vector<float> heights;
        std::vector<SizeF> controls;
        float total = 0;
        for (const Row& row : sec.rows) {
            const SizeF c = control_size(g, row);
            const float tw = cw - 2 * pad - (c.Width > 0 ? c.Width + px(16) : 0);
            float h = px(20);
            if (!row.detail.empty()) h += px(2) + text_height(g, row.detail, g_f.caption.get(), tw);
            heights.push_back((std::max)(px(52), h + px(24)));
            controls.push_back(c);
            total += heights.back();
        }
        const RectF card(m, y, cw, total);
        fill_round(g, card, px(10), g_t.card);
        stroke_round(g, card, px(10), g_t.border);
        float ry = y;
        for (size_t ri = 0; ri < sec.rows.size(); ++ri) {
            const Row& row = sec.rows[ri];
            const SizeF c = controls[ri];
            const float h = heights[ri];
            if (ri > 0) {
                Gdiplus::Pen line(g_t.border, 1);
                g.DrawLine(&line, m + pad, ry, m + cw - pad, ry);
            }
            const float tw = cw - 2 * pad - (c.Width > 0 ? c.Width + px(16) : 0);
            const float dh = row.detail.empty() ? 0 : px(2) + text_height(g, row.detail, g_f.caption.get(), tw);
            const float ty = ry + (h - px(20) - dh) / 2;
            draw_text(g, row.title, g_f.body.get(), row.destructive && row.kind == Row::kText ? g_t.danger : g_t.text,
                      RectF(m + pad, ty, tw, px(20)));
            if (!row.detail.empty()) {
                draw_text(g, row.detail, g_f.caption.get(), g_t.text2, RectF(m + pad, ty + px(22), tw, dh + px(2)),
                          kLeft, true, false);
            }
            if (c.Width > 0) {
                const RectF cr(m + cw - pad - c.Width, ry + (h - c.Height) / 2, c.Width, c.Height);
                const bool interactive = row.kind != Row::kText;
                const bool hot = interactive && s.hot == static_cast<int>(s.targets.size());
                paint_control(g, row, cr, hot);
                if (interactive) s.targets.push_back({cr, static_cast<int>(si), static_cast<int>(ri)});
            }
            ry += h;
        }
        y = ry + px(8);
        if (!sec.footer.empty()) {
            const float fh = text_height(g, sec.footer, g_f.caption.get(), cw - px(8));
            draw_text(g, sec.footer, g_f.caption.get(), g_t.text2, RectF(m + px(4), y, cw - px(8), fh + px(2)), kLeft,
                      true, false);
            y += fh + px(4);
        }
        y += px(18);
    }
    s.content_h = y + s.scroll;
    g.ResetClip();

    Gdiplus::SolidBrush bar(g_t.dark ? mix(g_t.bg, kBlack, 0.15f) : mix(g_t.bg, kWhite, 0.5f));
    g.FillRectangle(&bar, RectF(0, s.view_h, W, foot));
    Gdiplus::Pen line(g_t.border, 1);
    g.DrawLine(&line, 0.0f, s.view_h, W, s.view_h);
    s.done = RectF(W - m - px(100), s.view_h + (foot - px(34)) / 2, px(100), px(34));
    draw_button(g, s.done, tr(L"Done"), s.hot == -2, true, false);
}

int sheet_hit(const Sheet& s, float x, float y) {
    if (contains(s.done, x, y)) return -2;
    if (y >= s.view_h) return -1;
    for (size_t i = 0; i < s.targets.size(); ++i) {
        if (contains(s.targets[i].r, x, y)) return static_cast<int>(i);
    }
    return -1;
}

void sheet_click(Sheet& s, int hit) {
    if (hit == -2) {
        close_sheet();
        return;
    }
    if (hit < 0 || hit >= static_cast<int>(s.targets.size())) return;
    const Sheet::Target t = s.targets[static_cast<size_t>(hit)];
    const Row row = s.sections[static_cast<size_t>(t.section)].rows[static_cast<size_t>(t.row)];  // act may rebuild
    if (!row.act) return;
    if (row.kind == Row::kButton) {
        row.act(0);
    } else if (row.kind == Row::kToggle) {
        row.act(!row.on);
    } else if (row.kind == Row::kChoice) {
        HMENU menu = CreatePopupMenu();
        for (size_t i = 0; i < row.choices.size(); ++i) {
            AppendMenuW(menu, MF_STRING | (static_cast<int>(i) == row.choice ? MF_CHECKED : 0), i + 1,
                        row.choices[i].c_str());
        }
        POINT pt{static_cast<LONG>(t.r.X), static_cast<LONG>(t.r.GetBottom() + px(2))};
        ClientToScreen(s.hwnd, &pt);
        const int pick = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_TOPALIGN | menu_flags(true), pt.x, pt.y, 0, s.hwnd,
                                        nullptr);
        DestroyMenu(menu);
        if (pick > 0) row.act(pick - 1);
    }
    if (g_sheet == &s && IsWindow(s.hwnd)) {
        s.sections = s.build();
        s.targets.clear();
        s.hot = -1;
        s.content_h = 0;
        InvalidateRect(s.hwnd, nullptr, TRUE);
        UpdateWindow(s.hwnd);
    }
}

LRESULT CALLBACK sheet_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<Sheet*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            buffered_paint(hwnd, [&](Graphics& g, float w, float h) { paint_sheet(*s, g, w, h); });
            return 0;
        case WM_MOUSEMOVE: {
            if (!s->tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                s->tracking = TrackMouseEvent(&tme);
            }
            const int hot = sheet_hit(*s, static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
            if (hot != s->hot) {
                s->hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            s->tracking = false;
            s->hot = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, s->hot != -1 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP:
            sheet_click(*s, sheet_hit(*s, static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))));
            return 0;
        case WM_MOUSEWHEEL: {
            s->scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * px(60);
            s->scroll = (std::max)(0.0f, (std::min)(s->scroll, (std::max)(0.0f, s->content_h - s->view_h)));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE || wp == VK_RETURN) close_sheet();
            return 0;
        case WM_CLOSE:
            close_sheet();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Shows a sheet over the library and returns when it is closed.
void run_sheet(Sheet sheet) {
    if (g_sheet) return;
    g_sheet = &sheet;
    sheet.sections = sheet.build();
    RECT mr;
    GetWindowRect(g_main, &mr);
    const int w = ipx(580), h = (std::min)(ipx(680), static_cast<int>(mr.bottom - mr.top) - ipx(20));
    const int x = mr.left + (mr.right - mr.left - w) / 2, y = mr.top + (mr.bottom - mr.top - h) / 2;
    sheet.hwnd = CreateWindowExW(layout_style(), L"WinApkSheet", sheet.title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y,
                                 w, (std::max)(h, ipx(360)), g_main, nullptr, g_inst, &sheet);
    if (!sheet.hwnd) {
        g_sheet = nullptr;
        return;
    }
    theme_window(sheet.hwnd);
    EnableWindow(g_main, FALSE);
    ShowWindow(sheet.hwnd, SW_SHOW);
    MSG m;
    while (IsWindow(sheet.hwnd)) {
        const BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) {
            PostQuitMessage(static_cast<int>(m.wParam));
            close_sheet();
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(g_main, TRUE);
    g_sheet = nullptr;
    SetActiveWindow(g_main);
    InvalidateRect(g_main, nullptr, FALSE);
}

Row button_row(std::wstring title, std::wstring detail, std::wstring button, std::function<void()> act,
               bool destructive = false) {
    Row r;
    r.kind = Row::kButton;
    r.title = std::move(title);
    r.detail = std::move(detail);
    r.value = std::move(button);
    r.destructive = destructive;
    r.act = [act](int) { act(); };
    return r;
}

Row text_row(std::wstring title, std::wstring value, std::wstring detail = L"") {
    Row r;
    r.title = std::move(title);
    r.value = std::move(value);
    r.detail = std::move(detail);
    return r;
}

void apply_theme();
void apply_language(bool keep_selected = false);

std::vector<Section> settings_sections() {
    std::vector<Section> out;

    Row theme;
    theme.kind = Row::kChoice;
    theme.title = tr(L"Theme");
    theme.choices = {tr(L"Match Windows"), tr(L"Light"), tr(L"Dark")};
    theme.choice = static_cast<int>((std::min)(setting(L"Theme", 0), 2ul));
    theme.act = [](int i) {
        set_setting(L"Theme", static_cast<DWORD>(i));
        apply_theme();
    };
    Row lang;
    lang.kind = Row::kChoice;
    lang.title = tr(L"Language");
    lang.choices = {tr(L"Match Windows"), L"English", L"עברית", L"日本語"};
    lang.choice = static_cast<int>(g_language_pref);
    lang.act = [](int i) {
        const DWORD selected = static_cast<DWORD>(i);
        g_live_language = i;
        g_language_pref = selected;
        g_ui_language = i == 3 ? UiLanguage::Japanese
                               : (i == 2 ? UiLanguage::Hebrew : UiLanguage::English);
        g_rtl = g_ui_language == UiLanguage::Hebrew;
        g_japanese = g_ui_language == UiLanguage::Japanese;
        set_setting(L"Language", selected);
        apply_language(true);
        if (g_sheet && g_sheet->hwnd) UpdateWindow(g_sheet->hwnd);
        if (g_main) UpdateWindow(g_main);
    };
    out.push_back({tr(L"Appearance"), {theme, lang}, L""});

    Row types;
    if (is_default_handler()) {
        types = text_row(tr(L"Open Android app files with Win.apk"), tr(L"Default"));
        types.check = true;
    } else {
        types = button_row(tr(L"Open Android app files with Win.apk"), L"", tr(L"Make default"), [] {
            register_file_types(true);
        });
    }
    out.push_back({tr(L"File types"), {types},
                   tr(L"Covers .apk, .apkm, .xapk and .apks. Double-clicking one in Explorer installs it and opens it.")});

    Row size;
    size.kind = Row::kChoice;
    size.title = tr(L"Screen size");
    for (const auto& p : kPhonePresets) size.choices.push_back(tr(p.label));
    size.choice = static_cast<int>(setting(L"PhoneSize", 0));
    if (size.choice >= static_cast<int>(size.choices.size())) size.choice = 0;
    size.act = [](int i) { set_setting(L"PhoneSize", static_cast<DWORD>(i)); };
    Row orient;
    orient.kind = Row::kChoice;
    orient.title = tr(L"Orientation");
    orient.choices = {tr(L"Portrait"), tr(L"Landscape")};
    orient.choice = setting(L"Landscape", 0) ? 1 : 0;
    orient.act = [](int i) { set_setting(L"Landscape", static_cast<DWORD>(i)); };
    out.push_back({tr(L"Screen"), {size, orient},
                   tr(L"Used the next time an app opens, from here or from its shortcut. Sizes are in dp, as on a phone.")});

    Row desktop;
    desktop.kind = Row::kToggle;
    desktop.title = tr(L"Desktop shortcut for new apps");
    desktop.detail = tr(L"Every app also gets one in Start > Win.apk Apps.");
    desktop.on = setting(L"DesktopShortcut", 1) != 0;
    desktop.act = [](int on) { set_setting(L"DesktopShortcut", static_cast<DWORD>(on)); };
    Row redo = button_row(tr(L"Shortcuts"), tr(L"Make the Start menu shortcuts of every app again, with their icons."),
                          tr(L"Recreate"), [] {
                              for (const App& a : g_apps) make_app_shortcuts(a, false);
                              set_status(tr(L"The Start menu shortcuts were made again."), kSuccess);
                          });
    out.push_back({tr(L"Shortcuts"), {desktop, redo}, L""});

    out.push_back({tr(L"Logs"),
                   {button_row(tr(L"Diagnostics"),
                               tr(L"install-<app>.log records each install, last-run.log the app opened last."),
                               tr(L"View logs"), [] { show_logs(last_run_log()); }),
                    button_row(tr(L"Logs folder"), logs_dir().wstring(), tr(L"Open folder"), [] { open_path(logs_dir()); })},
                   tr(L"Attach the log to a bug report: it says what the runtime did and where it stopped.")});
    // Read by the runtime when an app opens (runtime_main.cpp, map_shared_folders).
    Row folders;
    folders.kind = Row::kToggle;
    folders.title = tr(L"Use my Windows folders");
    folders.detail = tr(L"Apps save to and open from your Downloads, Pictures, Documents, Music and Videos. Off: each "
                        L"folder is empty for apps again, and your files stay where they are.");
    folders.on = setting(L"UserFolders", 1) != 0;
    folders.act = [](int on) { set_setting(L"UserFolders", static_cast<DWORD>(on)); };
    out.push_back({tr(L"App data"),
                   {folders, button_row(tr(L"Saves, caches and files"),
                                        tr(L"Each installed app keeps its data here. Removing an app keeps what it saved."),
                                        tr(L"Open folder"), [] { open_path(data_dir()); })},
                   L""});

    Section about{tr(L"About"), {text_row(L"Win.apk", fmt(L"Version {0}", {version()}),
                                      tr(L"An unofficial port of Mac.apk to Windows. Not affiliated with or endorsed by the Mac.apk project."))},
                  L""};
    if (!g_apps.empty()) {
        about.rows.push_back(button_row(tr(L"Remove all apps"), tr(L"Their shortcuts go too. What they saved stays."),
                                        tr(L"Remove all…"), [] {
                                            if (!confirm(fmt(L"Remove all {0} apps?", {std::to_wstring(g_apps.size())}),
                                                         tr(L"Their shortcuts go too. What they saved stays."),
                                                         tr(L"Remove all"))) {
                                                return;
                                            }
                                            const std::vector<App> apps = g_apps;
                                            for (const App& a : apps) remove_app(a, false);
                                            refresh();
                                            set_status(tr(L"Every app was removed."), kInfo);
                                        },
                                        true));
    }
    out.push_back(std::move(about));
    return out;
}

void show_settings() {
    Sheet s;
    s.title = tr(L"Settings");
    s.build = settings_sections;
    run_sheet(std::move(s));
}

void show_info(const App& app) {
    const App a = app;
    std::error_code ec;
    const std::wstring apk_size = format_bytes(fs::file_size(a.apk, ec));
    const std::wstring files_size = format_bytes(folder_size(app_dir(a.name)));
    const std::wstring installed = file_date(label_file(a.name));
    Sheet s;
    s.title = a.label;
    s.hero_app = a.name;
    s.hero_title = a.label;
    s.hero_sub = fmt(L"{0}  ·  Android app", {a.name});
    s.build = [a, apk_size, files_size, installed] {
        std::vector<Section> out;
        out.push_back({tr(L"Identity"),
                       {text_row(tr(L"Name"), a.label), text_row(tr(L"Library name"), a.name),
                        text_row(tr(L"Installed"), installed)},
                       L""});
        out.push_back({tr(L"Storage"),
                       {button_row(fmt(L"Source APK  ·  {0}", {apk_size}), a.apk.wstring(), tr(L"Show"),
                                   [a] { show_in_explorer(a.apk); }),
                        button_row(fmt(L"App files  ·  {0}", {files_size}), app_dir(a.name).wstring(), tr(L"Open"),
                                   [a] { open_path(app_dir(a.name)); }),
                        button_row(tr(L"Install log"), install_log(a.name).wstring(), tr(L"View"),
                                   [a] { show_logs(install_log(a.name)); })},
                       L""});
        Row open = button_row(fmt(L"Open {0}", {a.label}), L"", tr(L"Open"), [a] {
            close_sheet();
            if (const App* cur = find_app(a.name)) launch(*cur);
        });
        open.primary = true;
        out.push_back({tr(L"Actions"),
                       {open,
                        button_row(tr(L"Desktop shortcut"), tr(L"Put a shortcut to the app on the desktop."), tr(L"Create"),
                                   [a] {
                                       make_app_shortcuts(a, true);
                                       set_status(fmt(L"A shortcut to {0} is on the desktop.", {a.label}), kSuccess);
                                   }),
                        button_row(tr(L"Update from APK"), tr(L"Install a newer version over this one. Saves stay."),
                                   tr(L"Choose…"),
                                   [a] {
                                       update_from_apk(a);
                                       if (is_pending(a.name)) close_sheet();
                                   }),
                        button_row(tr(L"Reinstall"), tr(L"Convert the app again from its APK."), tr(L"Reinstall"),
                                   [a] {
                                       close_sheet();
                                       install(a.apk, false, a.name);
                                   }),
                        button_row(tr(L"Remove"), tr(L"Its shortcuts go too. What the app saved stays."), tr(L"Remove…"),
                                   [a] {
                                       if (remove_app(a, true)) close_sheet();
                                   },
                                   true)},
                       L""});
        return out;
    };
    run_sheet(std::move(s));
}

// A store app's details, and its install, update or open.
void show_store_info(const StoreApp& app) {
    const StoreApp a = app;  // the catalog may be replaced while the sheet is open
    Sheet s;
    s.title = store_name(a);
    s.hero_title = store_name(a);
    s.hero_sub = store_summary(a);
    s.hero_icon = store_icon(app);
    s.build = [a] {
        std::wstring cats;
        for (const std::wstring& c : a.categories) cats += (cats.empty() ? L"" : L", ") + c;
        std::vector<Section> out;
        out.push_back({tr(L"Details"),
                       {text_row(tr(L"Version"), a.version),
                        text_row(tr(L"Size"), format_bytes(static_cast<uintmax_t>(a.size))),
                        text_row(tr(L"Updated"), java_date(a.updated)), text_row(tr(L"License"), a.license),
                        text_row(tr(L"Categories"), cats), text_row(tr(L"Package"), utf8_to_wide(a.pkg))},
                       L""});
        Row get;
        switch (offer_for(a)) {
            case kOfferInstall:
            case kOfferUpdate: {
                const bool update = offer_for(a) == kOfferUpdate;
                get = button_row(fmt(update ? L"Update {0}" : L"Install {0}", {store_name(a)}),
                                 tr(update ? L"A newer version is out. Saves stay."
                                           : L"Downloaded from F-Droid and checked, then added to the library."),
                                 tr(update ? L"Update" : L"Install"), [a] {
                                     store_get(a);
                                     close_sheet();
                                 });
                get.primary = true;
                break;
            }
            case kOfferOpen:
                get = button_row(fmt(L"Open {0}", {store_name(a)}), L"", tr(L"Open"), [a] {
                    close_sheet();
                    store_get(a);
                });
                get.primary = true;
                break;
            case kOfferDownloading: get = text_row(store_name(a), tr(L"Downloading…")); break;
            case kOfferInstalling: get = text_row(store_name(a), tr(L"Installing…")); break;
        }
        const std::wstring page = L"https://f-droid.org/packages/" + utf8_to_wide(a.pkg) + L"/";
        out.push_back({tr(L"Actions"),
                       {get, button_row(tr(L"F-Droid page"), tr(L"The app's description, screenshots and source code."),
                                        tr(L"Open"),
                                        [page] {
                                            ShellExecuteW(nullptr, L"open", page.c_str(), nullptr, nullptr,
                                                          SW_SHOWNORMAL);
                                        })},
                       L""});
        return out;
    };
    run_sheet(std::move(s));
}

// ---------------------------------------------------------------------------
// Shared by the install and log windows

Color code_bg() { return g_t.dark ? hex(0x141418) : hex(0xFAFAFC); }

// The end of a log, read while the runtime may still be writing it.
std::string read_tail(const fs::path& p, size_t max_bytes) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    LARGE_INTEGER start{};
    start.QuadPart = (std::max)(0LL, static_cast<long long>(size.QuadPart) - static_cast<long long>(max_bytes));
    SetFilePointerEx(f, start, nullptr, FILE_BEGIN);
    std::string out(static_cast<size_t>(size.QuadPart - start.QuadPart), '\0');
    DWORD got = 0;
    if (!out.empty()) ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &got, nullptr);
    out.resize(got);
    CloseHandle(f);
    if (start.QuadPart > 0) out.erase(0, out.find('\n') == std::string::npos ? 0 : out.find('\n') + 1);
    return out;
}

std::vector<std::wstring> last_lines(const fs::path& p, size_t n) {
    std::vector<std::wstring> lines(1);
    for (wchar_t c : utf8_to_wide(read_tail(p, 16384))) {
        if (c == L'\n') lines.emplace_back();
        else if (c != L'\r') lines.back() += c == L'\t' ? L' ' : c;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.size() > n) lines.erase(lines.begin(), lines.end() - static_cast<long>(n));
    return lines;
}

std::wstring duration(ULONGLONG ms) {
    const ULONGLONG s = ms / 1000;
    wchar_t buf[32];
    swprintf(buf, 32, L"%llu:%02llu", s / 60, s % 60);
    return buf;
}

using Targets = std::vector<std::pair<RectF, int>>;

int find_target(const Targets& targets, LPARAM lp) {
    const float x = static_cast<float>(GET_X_LPARAM(lp)), y = static_cast<float>(GET_Y_LPARAM(lp));
    for (const auto& t : targets) {
        if (contains(t.first, x, y)) return t.second;
    }
    return 0;
}

// Hover tracking for the custom-drawn buttons of a window; true when it changed.
bool track_hot(HWND hwnd, LPARAM lp, const Targets& targets, int* hot, bool* tracking) {
    if (!*tracking) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        *tracking = TrackMouseEvent(&tme);
    }
    const int now = find_target(targets, lp);
    if (now == *hot) return false;
    *hot = now;
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

HWND open_tool_window(const wchar_t* cls, const wchar_t* title, int w, int h) {
    RECT mr;
    GetWindowRect(g_main, &mr);
    const int x = mr.left + (mr.right - mr.left - w) / 2, y = mr.top + (std::max)(0L, (mr.bottom - mr.top - h) / 2);
    HWND hwnd = CreateWindowExW(layout_style(), cls, title, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, x, y, w, h, g_main, nullptr,
                                g_inst, nullptr);
    if (hwnd) {
        theme_window(hwnd);
        ShowWindow(hwnd, SW_SHOW);
    }
    return hwnd;
}

// ---------------------------------------------------------------------------
// The install window: the queue, each app's progress, and its log as it is written

struct InstallView {
    Targets targets;
    int hot = 0;
    bool tracking = false;
    std::vector<std::wstring> log;
    fs::path log_path;
    ULONGLONG log_read = 0;
} g_iv;

enum { kActClose = 1, kActLog = 2, kActOpen = 1000, kActJobLog = 2000 };

// The install being converted, else the one that finished last.
const Job* log_job() {
    for (const Job& j : g_jobs) {
        if (j.state == Job::kInstalling) return &j;
    }
    for (auto it = g_jobs.rbegin(); it != g_jobs.rend(); ++it) {
        if (it->state != Job::kWaiting) return &*it;
    }
    return nullptr;
}

void paint_install(Graphics& g, float W, float H) {
    g.Clear(g_t.bg);
    g_iv.targets.clear();
    const float m = px(24), foot = px(64), cw = W - 2 * m;
    int done = 0, failed = 0;
    for (const Job& j : g_jobs) done += j.state == Job::kDone, failed += j.state == Job::kFailed;
    const int total = static_cast<int>(g_jobs.size());
    const bool finished = g_pending.empty();
    const ULONGLONG now = GetTickCount64();

    std::wstring title, sub;
    if (!finished) {
        title = total > 1 ? fmt(L"Installing {0} of {1}",
                                {std::to_wstring((std::min)(done + failed + 1, total)), std::to_wstring(total)})
                          : std::wstring(tr(L"Installing"));
        sub = tr(L"Each app is converted for Windows once. That takes up to a minute.");
    } else if (failed == 0) {
        const App* app = total == 1 ? find_app(g_jobs[0].name) : nullptr;
        title = app ? fmt(L"{0} is ready", {app->label}) : std::wstring(tr(L"All set"));
        sub = fmt(done == 1 ? L"{0} app is in the library and in Start > Win.apk Apps."
                            : L"{0} apps are in the library and in Start > Win.apk Apps.",
                  {std::to_wstring(done)});
    } else {
        title = failed == total ? tr(L"The install failed") : tr(L"Some installs failed");
        sub = fmt(L"{0} of {1} could not be installed. The log says why.",
                  {std::to_wstring(failed), std::to_wstring(total)});
    }
    float y = px(22);
    const RectF badge(m, y, px(44), px(44));
    if (!finished) {
        fill_round(g, badge, px(22), with_alpha(g_t.accent, g_t.dark ? 50 : 28));
        spinner(g, inset(badge, px(10), px(10)), g_t.accent, px(3));
    } else {
        Gdiplus::SolidBrush b(failed ? g_t.danger : g_t.success);
        g.FillEllipse(&b, badge);
        draw_text(g, failed ? L"" : kGlyphCheck, g_f.glyph16.get(), kWhite, badge, kCenter);
    }
    draw_text(g, title, g_f.hero.get(), g_t.text, RectF(m + px(60), y - px(2), cw - px(60), px(28)));
    draw_text(g, sub, g_f.caption.get(), g_t.text2, RectF(m + px(60), y + px(26), cw - px(60), px(20)));
    y += px(70);

    const float rh = px(60);
    const int rows = (std::max)(1, (std::min)(total, static_cast<int>((H * 0.45f) / rh)));
    const RectF card(m, y, cw, rows * rh);
    fill_round(g, card, px(10), g_t.card);
    stroke_round(g, card, px(10), g_t.border);
    g.SetClip(card);
    const int first = (std::max)(0, (std::min)(total - rows, [&] {  // keep the active install in view
        for (int i = 0; i < total; ++i) {
            if (g_jobs[static_cast<size_t>(i)].state == Job::kInstalling) return i - rows + 2;
        }
        return total - rows;
    }()));
    for (int i = first; i < total && i < first + rows; ++i) {
        const Job& j = g_jobs[static_cast<size_t>(i)];
        const RectF r(m, y + (i - first) * rh, cw, rh);
        if (i > first) {
            Gdiplus::Pen line(g_t.border, 1);
            g.DrawLine(&line, r.X + px(66), r.Y, r.GetRight() - px(14), r.Y);
        }
        const App* app = find_app(j.name);
        const std::wstring label = job_label(j.name);
        draw_app_icon(g, app, label, RectF(r.X + px(14), r.Y + px(10), px(40), px(40)));
        std::wstring state;
        switch (j.state) {
            case Job::kWaiting: state = tr(L"Waiting"); break;
            case Job::kInstalling: state = fmt(L"Converting  ·  {0}", {duration(now - j.start)}); break;
            case Job::kDone: state = fmt(L"Installed in {0}", {duration(j.end - j.start)}); break;
            case Job::kFailed: state = fmt(L"Failed after {0}", {duration(j.end - j.start)}); break;
        }
        const float tx = r.X + px(66), tw = r.Width - px(66) - px(120);
        draw_text(g, label, g_f.body_semi.get(), g_t.text, RectF(tx, r.Y + px(10), tw, px(20)));
        draw_text(g, state, g_f.caption.get(), j.state == Job::kFailed ? g_t.danger : g_t.text2,
                  RectF(tx, r.Y + px(31), tw, px(18)));
        const RectF right(r.GetRight() - px(14) - px(96), r.Y + (rh - px(30)) / 2, px(96), px(30));
        const int act = j.state == Job::kDone ? kActOpen + i : j.state == Job::kFailed ? kActJobLog + i : 0;
        if (j.state == Job::kWaiting) {
            draw_text(g, kGlyphClock, g_f.glyph16.get(), g_t.text2, right, kRight);
        } else if (j.state == Job::kInstalling) {
            spinner(g, RectF(right.GetRight() - px(20), right.Y + px(5), px(20), px(20)), g_t.accent, px(2.5f));
        } else {
            draw_button(g, right, j.state == Job::kDone ? tr(L"Open") : tr(L"View log"), g_iv.hot == act, false,
                        j.state == Job::kFailed);
            g_iv.targets.push_back({right, act});
        }
    }
    g.ResetClip();
    y += rows * rh + px(20);

    const Job* lj = log_job();
    draw_text(g, lj ? fmt(L"Log  ·  {0}", {lj->name}) : std::wstring(tr(L"Log")), g_f.caption_semi.get(), g_t.text2,
              RectF(m + px(4), y, cw, px(18)));
    y += px(24);
    const RectF box(m, y, cw, H - foot - px(18) - y);
    if (box.Height > px(48)) {
        fill_round(g, box, px(10), code_bg());
        stroke_round(g, box, px(10), g_t.border);
        const float lh = px(17);
        const size_t fit = static_cast<size_t>((std::max)(0.0f, (box.Height - px(20)) / lh));
        if (g_iv.log.empty()) {
            draw_text(g, lj ? tr(L"Starting the runtime…") : tr(L"The install's output appears here."), g_f.caption.get(),
                      g_t.text2, box, kCenter);
        } else {
            const size_t from = g_iv.log.size() > fit ? g_iv.log.size() - fit : 0;
            float ly = box.Y + px(10);
            for (size_t i = from; i < g_iv.log.size(); ++i, ly += lh) {
                const std::wstring& l = g_iv.log[i];
                const bool bad = l.find(L"rror") != std::wstring::npos || l.find(L"xception") != std::wstring::npos;
                draw_text(g, l, g_f.mono.get(), bad ? g_t.danger : g_t.text2,
                          RectF(box.X + px(12), ly, box.Width - px(24), lh), kLeft, false, true, true);
            }
        }
    }

    Gdiplus::SolidBrush bar(g_t.dark ? mix(g_t.bg, kBlack, 0.15f) : mix(g_t.bg, kWhite, 0.5f));
    g.FillRectangle(&bar, RectF(0, H - foot, W, foot));
    Gdiplus::Pen line(g_t.border, 1);
    g.DrawLine(&line, 0.0f, H - foot, W, H - foot);
    const RectF close(W - m - px(100), H - foot + (foot - px(34)) / 2, px(100), px(34));
    draw_button(g, close, finished ? tr(L"Done") : tr(L"Hide"), g_iv.hot == kActClose, true, false);
    g_iv.targets.push_back({close, kActClose});
    if (lj) {
        const RectF full(m, close.Y, px(140), px(34));
        draw_button(g, full, tr(L"Open full log"), g_iv.hot == kActLog, false, false);
        g_iv.targets.push_back({full, kActLog});
    }
}

void install_action(HWND hwnd, int act) {
    if (act == kActClose) {
        DestroyWindow(hwnd);
    } else if (act == kActLog) {
        if (const Job* j = log_job()) show_logs(install_log(j->name));
    } else if (act >= kActJobLog) {
        const size_t i = static_cast<size_t>(act - kActJobLog);
        if (i < g_jobs.size()) show_logs(install_log(g_jobs[i].name));
    } else if (act >= kActOpen) {
        const size_t i = static_cast<size_t>(act - kActOpen);
        if (i < g_jobs.size()) {
            if (const App* a = find_app(g_jobs[i].name)) launch(*a);
        }
    }
}

LRESULT CALLBACK install_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: SetTimer(hwnd, 1, 33, nullptr); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: buffered_paint(hwnd, paint_install); return 0;
        case WM_TIMER: {
            const Job* j = log_job();
            if (j && GetTickCount64() - g_iv.log_read > 400) {
                g_iv.log = last_lines(install_log(j->name), 80);
                g_iv.log_read = GetTickCount64();
            } else if (!j) {
                g_iv.log.clear();
            }
            if (!g_pending.empty() || g_iv.log_read + 1000 > GetTickCount64()) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: track_hot(hwnd, lp, g_iv.targets, &g_iv.hot, &g_iv.tracking); return 0;
        case WM_MOUSELEAVE:
            g_iv.tracking = false;
            g_iv.hot = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, g_iv.hot ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP: install_action(hwnd, find_target(g_iv.targets, lp)); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE || wp == VK_RETURN) DestroyWindow(hwnd);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ipx(420), ipx(440)};
            return 0;
        case WM_DESTROY:
            g_install_wnd = nullptr;
            g_iv = InstallView{};
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_install_window() {
    if (g_install_wnd) {
        InvalidateRect(g_install_wnd, nullptr, FALSE);
        return;
    }
    g_install_wnd = open_tool_window(L"WinApkInstall", tr(L"Installing - Win.apk"), ipx(540), ipx(600));
}

// ---------------------------------------------------------------------------
// The log window: any of the logs, following the end as the runtime writes it

struct LogView {
    fs::path path;
    uintmax_t size = static_cast<uintmax_t>(-1);
    Targets targets;
    int hot = 0;
    bool tracking = false;
} g_lv;

enum { kLogPick = 1, kLogCopy, kLogNotepad, kLogFolder };

std::wstring log_title(const fs::path& p) {
    const std::wstring stem = p.stem().wstring();
    if (stem == L"last-run") return tr(L"Last run");
    if (stem.rfind(L"install-", 0) == 0) return fmt(L"Install  ·  {0}", {stem.substr(8)});
    return stem;
}

std::vector<fs::path> log_files() {
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(logs_dir(), ec)) {
        if (e.path().extension() == L".log") found.push_back({e.last_write_time(ec), e.path()});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

void load_log(bool force) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(g_lv.path, ec);
    if (!force && (ec ? 0 : size) == g_lv.size) return;
    g_lv.size = ec ? 0 : size;
    std::wstring text;
    if (ec) {
        text = g_lv.path.stem() == L"last-run" ? tr(L"No app has run yet. This log is written when one does.")
                                               : tr(L"This log is written the next time the app is installed.");
    } else {
        const std::wstring raw = utf8_to_wide(read_tail(g_lv.path, 1 << 20));
        text.reserve(raw.size() + raw.size() / 32);
        for (wchar_t c : raw) {
            if (c == L'\n') text += L"\r\n";
            else if (c != L'\r') text += c;
        }
    }
    SCROLLINFO si{sizeof(si), SIF_ALL};
    GetScrollInfo(g_log_edit, SB_VERT, &si);
    const bool at_end = force || si.nPos + static_cast<int>(si.nPage) >= si.nMax;
    const LRESULT first = SendMessageW(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SetWindowTextW(g_log_edit, text.c_str());
    if (at_end) {
        const int len = GetWindowTextLengthW(g_log_edit);
        SendMessageW(g_log_edit, EM_SETSEL, len, len);
        SendMessageW(g_log_edit, EM_SCROLLCARET, 0, 0);
        SendMessageW(g_log_edit, WM_HSCROLL, SB_LEFT, 0);  // the caret's scroll may have moved sideways too
    } else {
        SendMessageW(g_log_edit, EM_LINESCROLL, 0, first);
    }
    InvalidateRect(g_log_wnd, nullptr, FALSE);
}

RectF log_text_rect(float W, float H) { return RectF(px(20), px(64), W - px(40), H - px(64) - px(40)); }

void paint_logs(Graphics& g, float W, float H) {
    g.Clear(g_t.bg);
    g_lv.targets.clear();
    const float m = px(20);
    draw_text(g, tr(L"Logs"), g_f.hero.get(), g_t.text, RectF(m, px(14), px(70), px(36)));
    const RectF pick(m + px(76), px(16), (std::min)(px(300), W * 0.4f), px(32));
    fill_round(g, pick, px(6), g_lv.hot == kLogPick ? g_t.hover : mix(g_t.card, g_t.hover, 0.55f));
    stroke_round(g, pick, px(6), g_t.border);
    draw_text(g, log_title(g_lv.path), g_f.body.get(), g_t.text, RectF(pick.X + px(12), pick.Y, pick.Width - px(40), pick.Height));
    draw_text(g, kGlyphChevron, g_f.glyph12.get(), g_t.text2, RectF(pick.GetRight() - px(30), pick.Y, px(22), pick.Height),
              kCenter);
    g_lv.targets.push_back({pick, kLogPick});
    float x = W - m;
    for (auto [label, act] : {std::pair<const wchar_t*, int>{tr(L"Folder"), kLogFolder}, {tr(L"Notepad"), kLogNotepad},
                              {tr(L"Copy"), kLogCopy}}) {
        const float w = text_width(g, label, g_f.body_semi.get()) + px(32);
        x -= w;
        const RectF r(x, px(16), w, px(32));
        if (r.X > pick.GetRight() + px(8)) {
            draw_button(g, r, label, g_lv.hot == act, false, false);
            g_lv.targets.push_back({r, act});
        }
        x -= px(8);
    }
    const RectF box = log_text_rect(W, H);
    fill_round(g, box, px(10), code_bg());
    stroke_round(g, box, px(10), g_t.border);
    const bool live = !g_running.empty() || !g_pending.empty();
    float fx = m;
    if (live) {
        Gdiplus::SolidBrush dot(g_t.success);
        g.FillEllipse(&dot, fx, H - px(20) - px(3), px(6), px(6));
        fx += px(14);
    }
    const std::wstring info = (live ? std::wstring(tr(L"Live")) + L"  ·  " : std::wstring()) + format_bytes(g_lv.size) + L"  ·  " + g_lv.path.wstring();
    draw_text(g, info, g_f.caption.get(), g_t.text2, RectF(fx, H - px(40), W - fx - m, px(40)));
}

void log_action(HWND hwnd, int act) {
    if (act == kLogPick) {
        const auto files = log_files();
        if (files.empty()) return;
        HMENU menu = CreatePopupMenu();
        for (size_t i = 0; i < files.size() && i < 40; ++i) {
            AppendMenuW(menu, MF_STRING | (files[i] == g_lv.path ? MF_CHECKED : 0), i + 1, log_title(files[i]).c_str());
        }
        const RectF& r = g_lv.targets[0].first;
        POINT pt{static_cast<LONG>(r.X), static_cast<LONG>(r.GetBottom() + px(2))};
        ClientToScreen(hwnd, &pt);
        const int pick = TrackPopupMenu(menu, TPM_RETURNCMD | menu_flags(true), pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (pick > 0) {
            g_lv.path = files[static_cast<size_t>(pick - 1)];
            load_log(true);
        }
    } else if (act == kLogCopy) {
        const int len = GetWindowTextLengthW(g_log_edit);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(len) + 1) * sizeof(wchar_t));
        if (!mem || !OpenClipboard(hwnd)) {
            if (mem) GlobalFree(mem);
            return;
        }
        GetWindowTextW(g_log_edit, static_cast<wchar_t*>(GlobalLock(mem)), len + 1);
        GlobalUnlock(mem);
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        CloseClipboard();
        set_status(tr(L"The log was copied."), kSuccess);
    } else if (act == kLogNotepad) {
        open_log(g_lv.path);
    } else if (act == kLogFolder) {
        std::error_code ec;
        if (fs::exists(g_lv.path, ec)) show_in_explorer(g_lv.path);
        else open_path(logs_dir());
    }
}

LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            // A log is English, paths and stack traces: it reads left to right, lines flush left, with its
            // scroll bar on the right, also in a Hebrew window. A child takes the mirrored layout of its
            // parent when it is created, and Windows keeps an edit as it was made (taking WS_EX_LAYOUTRTL off
            // afterwards still leaves the lines on the right), so the window stops passing it on first.
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_NOINHERITLAYOUT);
            g_log_edit = CreateWindowExW(WS_EX_LTRREADING | WS_EX_LEFT | WS_EX_RIGHTSCROLLBAR, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE |
                                             ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                         0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
            SendMessageW(g_log_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_mono_font), FALSE);
            SendMessageW(g_log_edit, EM_SETLIMITTEXT, 0, 0);
            SetWindowTheme(g_log_edit, g_t.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            SetTimer(hwnd, 1, 700, nullptr);
            return 0;
        case WM_SIZE: {
            const RectF r = inset(log_text_rect(LOWORD(lp), HIWORD(lp)), px(10), px(8));
            MoveWindow(g_log_edit, static_cast<int>(r.X), static_cast<int>(r.Y), static_cast<int>(r.Width),
                       static_cast<int>(r.Height), TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: buffered_paint(hwnd, paint_logs); return 0;
        case WM_CTLCOLORSTATIC:  // a read-only edit asks with this one
            SetTextColor(reinterpret_cast<HDC>(wp), colorref(g_t.text));
            SetBkColor(reinterpret_cast<HDC>(wp), colorref(code_bg()));
            return reinterpret_cast<LRESULT>(g_code_brush);
        case WM_TIMER: load_log(false); return 0;
        case WM_MOUSEMOVE: track_hot(hwnd, lp, g_lv.targets, &g_lv.hot, &g_lv.tracking); return 0;
        case WM_MOUSELEAVE:
            g_lv.tracking = false;
            g_lv.hot = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd) {
                SetCursor(LoadCursorW(nullptr, g_lv.hot ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP: log_action(hwnd, find_target(g_lv.targets, lp)); return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ipx(560), ipx(360)};
            return 0;
        case WM_DESTROY:
            g_log_wnd = g_log_edit = nullptr;
            g_lv.targets.clear();
            g_lv.hot = 0;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_logs(const fs::path& log) {
    g_lv.path = log;
    if (!g_log_wnd) {
        g_log_wnd = open_tool_window(L"WinApkLogs", tr(L"Logs - Win.apk"), ipx(860), ipx(620));
        if (!g_log_wnd) return;
    } else {
        ShowWindow(g_log_wnd, SW_RESTORE);
        SetForegroundWindow(g_log_wnd);
    }
    load_log(true);
}

// ---------------------------------------------------------------------------
// Drag and drop, through OLE so the drop zone can light up while dragging

std::vector<fs::path> dropped_files(IDataObject* data) {
    std::vector<fs::path> out;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m{};
    if (FAILED(data->GetData(&fe, &m))) return out;
    if (auto drop = static_cast<HDROP>(GlobalLock(m.hGlobal))) {
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH * 4];
            if (DragQueryFileW(drop, i, path, MAX_PATH * 4) && is_app_file(path)) out.emplace_back(path);
        }
        GlobalUnlock(m.hGlobal);
    }
    ReleaseStgMedium(&m);
    return out;
}

void set_view(View view);

class DropTarget : public IDropTarget {
public:
    virtual ~DropTarget() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        accept_ = !g_sheet && !dropped_files(data).empty();
        if (accept_) set_view(kLibraryView);  // where the drop zone is
        set_over(accept_);
        *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        set_over(false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        set_over(false);
        *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (accept_) {
            for (const fs::path& p : dropped_files(data)) install(p, false);
        }
        return S_OK;
    }

private:
    static void set_over(bool over) {
        g_drag_over = over;
        InvalidateRect(g_main, nullptr, FALSE);
    }
    LONG refs_ = 1;
    bool accept_ = false;
};

DropTarget* g_drop_target;

// ---------------------------------------------------------------------------
// The library window

void apply_theme() {
    pick_theme();
    theme_window(g_main);
    if (sheet_hwnd()) {
        theme_window(sheet_hwnd());
        InvalidateRect(sheet_hwnd(), nullptr, FALSE);
    }
    theme_menus();
    if (g_edit_brush) DeleteObject(g_edit_brush);
    g_edit_brush = CreateSolidBrush(colorref(g_t.card));
    if (g_code_brush) DeleteObject(g_code_brush);
    g_code_brush = CreateSolidBrush(colorref(code_bg()));
    for (HWND w : {g_install_wnd, g_log_wnd}) {
        if (!w) continue;
        theme_window(w);
        InvalidateRect(w, nullptr, FALSE);
    }
    if (g_log_edit) {
        SetWindowTheme(g_log_edit, g_t.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        InvalidateRect(g_log_edit, nullptr, TRUE);
    }
    InvalidateRect(g_main, nullptr, FALSE);
    InvalidateRect(g_search, nullptr, TRUE);
}

void show_context_menu(const App& app, POINT pt) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdOpen, tr(L"&Open"));
    AppendMenuW(menu, MF_STRING, kCmdInfo, tr(L"Get &Info…"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdShortcut, tr(L"Create &desktop shortcut"));
    AppendMenuW(menu, MF_STRING, kCmdShowApk, tr(L"Show the &APK in Explorer"));
    AppendMenuW(menu, MF_STRING, kCmdAppFolder, tr(L"Open the app's &files"));
    AppendMenuW(menu, MF_STRING, kCmdLog, tr(L"Show the last &log"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdUpdate, tr(L"&Update from APK…"));
    AppendMenuW(menu, MF_STRING, kCmdReinstall, tr(L"Rein&stall"));
    AppendMenuW(menu, MF_STRING, kCmdRemove, tr(L"&Remove…"));
    SetMenuDefaultItem(menu, kCmdOpen, FALSE);
    if (is_pending(app.name)) {
        for (int id : {kCmdOpen, kCmdUpdate, kCmdReinstall, kCmdRemove}) EnableMenuItem(menu, id, MF_GRAYED);
    }
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | menu_flags(false), pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(menu);
}

void command(int id) {
    const App* sel = selected();
    if (!sel) return;
    const App a = *sel;
    switch (id) {
        case kCmdOpen: launch(a); break;
        case kCmdInfo: show_info(a); break;
        case kCmdShortcut:
            make_app_shortcuts(a, true);
            set_status(fmt(L"A shortcut to {0} is on the desktop.", {a.label}), kSuccess);
            break;
        case kCmdShowApk: show_in_explorer(a.apk); break;
        case kCmdAppFolder: open_path(app_dir(a.name)); break;
        case kCmdLog: show_logs(last_run_log()); break;
        case kCmdUpdate: update_from_apk(a); break;
        case kCmdReinstall: install(a.apk, false, a.name); break;
        case kCmdRemove: remove_app(a, true); break;
    }
}

void move_selection(int dx, int dy) {
    int i = tile_of(g_selected);
    if (i < 0) {
        for (size_t t = 0; t < g_tiles.size() && i < 0; ++t) {
            if (g_tiles[t].app >= 0) i = static_cast<int>(t);
        }
    } else {
        i += dx + dy * g_l.cols;
    }
    if (i < 0 || i >= static_cast<int>(g_tiles.size()) || g_tiles[static_cast<size_t>(i)].app < 0) return;
    select_app(g_apps[static_cast<size_t>(g_tiles[static_cast<size_t>(i)].app)].name);
}

LRESULT CALLBACK search_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        SetWindowTextW(hwnd, L"");
        SetFocus(g_main);
        return 0;
    }
    if (msg == WM_KEYDOWN && (wp == VK_DOWN || wp == VK_RETURN)) {
        SetFocus(g_main);
        if (g_view == kLibraryView && !selected()) move_selection(0, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;  // no beep
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// The search box, made again when the language changes so it takes the new direction.
void create_search() {
    wchar_t text[256] = {};
    if (g_search) {
        GetWindowTextW(g_search, text, 256);
        DestroyWindow(g_search);
    }
    g_search = CreateWindowExW(g_rtl ? WS_EX_RTLREADING : 0, L"EDIT", text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                               0, 0, 0, 0, g_main, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearch)), g_inst,
                               nullptr);
    SendMessageW(g_search, WM_SETFONT, reinterpret_cast<WPARAM>(g_edit_font), FALSE);
    SendMessageW(g_search, EM_SETCUEBANNER, FALSE,
                 reinterpret_cast<LPARAM>(tr(g_view == kStoreView ? L"Search F-Droid" : L"Search apps")));
    SetWindowSubclass(g_search, search_proc, 1, 0);
}

void set_view(View view) {
    if (view == g_view) return;
    const std::wstring shown_query = query(g_view);
    g_view = view;
    std::swap(g_scroll, g_other_scroll);
    const float scroll = g_scroll;
    SetWindowTextW(g_search, g_other_query.c_str());  // its EN_CHANGE starts at the top; this view keeps its place
    g_other_query = shown_query;
    g_scroll = scroll;
    SendMessageW(g_search, EM_SETCUEBANNER, FALSE,
                 reinterpret_cast<LPARAM>(tr(view == kStoreView ? L"Search F-Droid" : L"Search apps")));
    InvalidateRect(g_search, nullptr, TRUE);  // an empty box shows the new cue only when it is drawn again
    g_hot = {};
    if (view == kStoreView && !g_store.started) store_start(false);
    rebuild_tiles();
    update_timer();
}

void apply_language(bool keep_selected) {
    if (!keep_selected) pick_language();
    for (HWND w : {g_main, sheet_hwnd(), g_install_wnd}) {
        if (!w) continue;
        const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        SetWindowLongPtrW(w, GWL_EXSTYLE, g_rtl ? (ex | WS_EX_LAYOUTRTL) : (ex & ~WS_EX_LAYOUTRTL));
        SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        InvalidateRect(w, nullptr, FALSE);
    }
    if (g_sheet && g_sheet->hwnd) {
        // Section and Row labels are materialized when a sheet opens. Rebuild
        // them after changing the language; repainting the old rows only kept
        // the previous English/Hebrew strings on screen.
        if (g_sheet->build) g_sheet->sections = g_sheet->build();
        g_sheet->targets.clear();
        g_sheet->scroll = 0;
        g_sheet->content_h = 0;
        SetWindowTextW(g_sheet->hwnd, tr(L"Settings"));
        InvalidateRect(g_sheet->hwnd, nullptr, TRUE);
        UpdateWindow(g_sheet->hwnd);
    }
    if (g_install_wnd) SetWindowTextW(g_install_wnd, tr(L"Installing - Win.apk"));
    if (g_log_wnd) DestroyWindow(g_log_wnd);
    create_search();
    rebuild_tiles();
}

void set_hot(const Hit& h) {
    if (h == g_hot) return;
    g_hot = h;
    InvalidateRect(g_main, nullptr, FALSE);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_main = hwnd;
            pick_language();
            // Load Language before creating any cached/main-window text.
            // Without this, g_language_pref remained its default 0 and an
            // explicitly selected Japanese GUI was painted in English.
            pick_language();
            g_dpi = static_cast<float>(GetDpiForWindow(hwnd));
            make_fonts();
            create_search();
            apply_theme();
            g_drop_target = new DropTarget;
            RegisterDragDrop(hwnd, g_drop_target);
            refresh();
            return 0;
        }
        case WM_SIZE:
            compute_layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            make_fonts();
            SendMessageW(g_search, WM_SETFONT, reinterpret_cast<WPARAM>(g_edit_font), FALSE);
            g_icons.clear();
            g_store.icons.clear();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            compute_layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) apply_theme();
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: buffered_paint(hwnd, paint_main); return 0;
        case WM_TIMER:
            if (wp == kAnimTimer) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wp), colorref(g_t.text));
            SetBkColor(reinterpret_cast<HDC>(wp), colorref(g_t.card));
            return reinterpret_cast<LRESULT>(g_edit_brush);
        case WM_COMMAND:
            if (LOWORD(wp) == kIdSearch) {
                if (HIWORD(wp) == EN_CHANGE) {
                    g_scroll = 0;
                    rebuild_tiles();
                } else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            command(LOWORD(wp));
            return 0;
        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lp)), y = static_cast<float>(GET_Y_LPARAM(lp));
            if (!g_tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                g_tracking = TrackMouseEvent(&tme);
            }
            if (g_thumb_drag) {
                const float room = g_l.grid.Height - thumb_rect().Height;
                if (room > 0) g_scroll = g_thumb_scroll0 + (y - g_thumb_y0) * g_l.max_scroll / room;
                clamp_scroll();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            set_hot(hit_test(x, y));
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (!g_thumb_drag) set_hot({});
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd) {
                const bool hand = g_hot.kind == kHitGear || g_hot.kind == kHitDrop || g_hot.kind == kHitLogs ||
                                  g_hot.kind == kHitStop || g_hot.kind == kHitTab || g_hot.kind == kHitCategory ||
                                  g_hot.kind == kHitCard || g_hot.kind == kHitCardButton || g_hot.kind == kHitRetry ||
                                  (g_hot.kind == kHitTile && g_tiles[static_cast<size_t>(g_hot.tile)].app == kAddTile);
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            const float x = static_cast<float>(GET_X_LPARAM(lp)), y = static_cast<float>(GET_Y_LPARAM(lp));
            const Hit h = hit_test(x, y);
            switch (h.kind) {
                case kHitGear: show_settings(); break;
                case kHitTab: set_view(h.tile == 1 ? kStoreView : kLibraryView); break;
                case kHitCategory: choose_category(); break;
                case kHitRetry: store_start(true); break;
                case kHitCardButton:
                case kHitCard: {
                    // a copy: opening the app or the sheet runs messages that may replace the catalog
                    const StoreApp a = g_store.apps[static_cast<size_t>(g_store.shown[static_cast<size_t>(h.tile)])];
                    if (h.kind == kHitCardButton) store_get(a);
                    else show_store_info(a);
                    break;
                }
                case kHitLogs: {
                    const auto logs = log_files();
                    show_logs(logs.empty() ? last_run_log() : logs.front());
                    break;
                }
                case kHitStop:
                    if (const App* a = running_app()) {
                        if (stop_process(a->name)) g_stopping.insert(a->name);
                    }
                    break;
                case kHitDrop: add_from_dialog(); break;
                case kHitThumb:
                    g_thumb_drag = true;
                    g_thumb_y0 = y;
                    g_thumb_scroll0 = g_scroll;
                    SetCapture(hwnd);
                    break;
                case kHitTrack:
                    g_scroll += (y < thumb_rect().Y ? -1 : 1) * g_l.grid.Height * 0.9f;
                    clamp_scroll();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                case kHitTile: {
                    const Tile& t = g_tiles[static_cast<size_t>(h.tile)];
                    if (t.app >= 0) select_app(g_apps[static_cast<size_t>(t.app)].name);
                    else if (t.app == kAddTile) add_from_dialog();
                    break;
                }
                case kHitNone:
                    g_selected.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_thumb_drag) {
                g_thumb_drag = false;
                ReleaseCapture();
                set_hot(hit_test(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))));
            }
            return 0;
        case WM_LBUTTONDBLCLK: {
            const Hit h = hit_test(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
            if (h.kind == kHitTile && g_tiles[static_cast<size_t>(h.tile)].app >= 0) {
                launch(g_apps[static_cast<size_t>(g_tiles[static_cast<size_t>(h.tile)].app)]);
            } else if (h.kind == kHitDrop) {
                add_from_dialog();
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            const Hit h = hit_test(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
            if (h.kind != kHitTile || g_tiles[static_cast<size_t>(h.tile)].app < 0) return 0;
            const App& app = g_apps[static_cast<size_t>(g_tiles[static_cast<size_t>(h.tile)].app)];
            select_app(app.name);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            show_context_menu(app, pt);
            return 0;
        }
        case WM_CONTEXTMENU:
            if (GET_X_LPARAM(lp) == -1 && selected()) {  // the keyboard's menu key
                const RectF r = tile_rect(tile_of(g_selected));
                POINT pt{static_cast<LONG>(r.X + r.Width / 2), static_cast<LONG>(r.Y + r.Height / 2)};
                ClientToScreen(hwnd, &pt);
                show_context_menu(*selected(), pt);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * px(60);
            clamp_scroll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN: {
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (ctrl && (wp == '1' || wp == '2')) {
                set_view(wp == '2' ? kStoreView : kLibraryView);
                return 0;
            }
            if (g_view == kStoreView) {
                switch (wp) {
                    case VK_UP:
                    case VK_DOWN:
                    case VK_PRIOR:
                    case VK_NEXT: {
                        const float page = wp == VK_PRIOR || wp == VK_NEXT ? g_l.grid.Height * 0.9f : px(60);
                        g_scroll += wp == VK_UP || wp == VK_PRIOR ? -page : page;
                        clamp_scroll();
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    case VK_F5: store_start(true); return 0;
                    case 'F': if (ctrl) SetFocus(g_search); return 0;
                    case VK_OEM_COMMA: if (ctrl) show_settings(); return 0;
                }
                break;
            }
            switch (wp) {
                case VK_LEFT: move_selection(-1, 0); return 0;
                case VK_RIGHT: move_selection(1, 0); return 0;
                case VK_UP: move_selection(0, -1); return 0;
                case VK_DOWN: move_selection(0, 1); return 0;
                case VK_RETURN: if (const App* a = selected()) launch(*a); return 0;
                case VK_DELETE: if (const App* a = selected()) remove_app(*a, true); return 0;
                case VK_F5: refresh(); return 0;
                case VK_ESCAPE:
                    g_selected.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case 'F': if (ctrl) SetFocus(g_search); return 0;
                case 'O': if (ctrl) add_from_dialog(); return 0;
                case 'I': if (ctrl && selected()) show_info(*selected()); return 0;
                case VK_OEM_COMMA: if (ctrl) show_settings(); return 0;
            }
            break;
        }
        case WM_CHAR:  // typing in the library searches it
            if (wp > 32 && GetKeyState(VK_CONTROL) >= 0) {
                SetFocus(g_search);
                SendMessageW(g_search, WM_CHAR, wp, lp);
            }
            return 0;
        case kInstallStarted: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            g_current = *name;
            for (Job& j : g_jobs) {
                if (j.name == *name && j.state == Job::kWaiting) {
                    j.state = Job::kInstalling;
                    j.start = GetTickCount64();
                    break;
                }
            }
            if (g_install_wnd) InvalidateRect(g_install_wnd, nullptr, FALSE);
            set_status(fmt(L"Installing {0}… The first install of an app takes up to a minute.", {job_label(*name)}),
                       kInfo);
            return 0;
        }
        case kInstalled: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            const auto it = std::find(g_pending.begin(), g_pending.end(), *name);
            if (it != g_pending.end()) g_pending.erase(it);
            ++g_batch_done;
            if (g_pending.empty()) g_current.clear();
            for (Job& j : g_jobs) {
                if (j.name == *name && j.state == Job::kInstalling) {
                    j.state = (wp & 1) ? Job::kDone : Job::kFailed;
                    j.end = GetTickCount64();
                    break;
                }
            }
            if (g_install_wnd) InvalidateRect(g_install_wnd, nullptr, FALSE);
            update_timer();
            refresh();
            if (wp & 1) {
                const std::wstring label = label_of(*name);
                select_app(*name);
                set_status(fmt(setting(L"DesktopShortcut", 1)
                                   ? L"{0} is installed: it is in the Start menu and on the desktop."
                                   : L"{0} is installed: it is in the Start menu.",
                               {label}),
                           kSuccess);
                if (wp & 2) {
                    if (const App* a = find_app(*name)) launch(*a);
                }
            } else if (wp & 4) {
                set_status(fmt(L"Close {0} before updating it.", {label_of(*name)}), kError);
            } else {
                set_status(fmt(L"Installing {0} failed.", {job_label(*name)}), kError);
                if (!g_install_wnd) {
                    failed_with_log(fmt(L"Installing {0} failed", {job_label(*name)}), install_log(*name));
                }
            }
            if (g_pending.empty()) g_job_labels.clear();
            return 0;
        }
        case kRunExited: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            g_running.erase(*name);
            update_timer();
            const std::wstring label = label_of(*name);
            if (g_stopping.erase(*name)) {
                set_status(fmt(L"{0} was stopped.", {label}), kInfo);
            } else if (wp) {
                // Exit code 0 is a normal app exit. Do not infer startup failure from
                // the final log after the window and graphics resources were destroyed.
                set_status(L"", kHint);
            } else {
                set_status(fmt(L"{0} stopped with an error.", {label}), kError);
                failed_with_log(fmt(L"{0} stopped with an error", {label}), last_run_log());
            }
            return 0;
        }
        case kRefresh:
            refresh();
            return 0;
        case kStoreCatalog:
            if (wp == kCatalogList) {
                std::unique_ptr<std::vector<StoreApp>> list(reinterpret_cast<std::vector<StoreApp>*>(lp));
                g_store.apps = std::move(*list);
                std::stable_sort(g_store.apps.begin(), g_store.apps.end(),
                                 [](const StoreApp& a, const StoreApp& b) { return a.updated > b.updated; });
                rebuild_tiles();
            } else {
                g_store.loading = false;
                g_store.failed = wp == kCatalogFailed;
                if (g_store.failed && !g_store.apps.empty() && g_view == kStoreView) {
                    set_status(tr(L"F-Droid could not be checked for new apps."), kError);
                }
                update_timer();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case kStoreProgress:
            g_store.got_kb = wp;
            return 0;
        case kStoreIcon: {
            std::unique_ptr<std::string> pkg(reinterpret_cast<std::string*>(lp));
            g_store.icon_wait.erase(*pkg);
            if (wp == kIconFailed) g_store.icons[*pkg] = nullptr;  // the monogram, for this session
            if (g_view == kStoreView) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case kStoreDownload: {
            std::unique_ptr<std::string> pkg(reinterpret_cast<std::string*>(lp));
            g_store.downloads[*pkg] = static_cast<int>(wp);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case kStoreDownloaded: {
            std::unique_ptr<Download> d(reinterpret_cast<Download*>(lp));
            store_downloaded(*d, wp != 0);
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {ipx(680), ipx(520)};
            return 0;
        case WM_DESTROY:
            RevokeDragDrop(hwnd);
            if (g_drop_target) g_drop_target->Release();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// The installed-apps registry

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

// The lines of library\<name>.package: package, versionCode, versionName.
std::vector<std::string> package_of(const std::wstring& name) {
    std::ifstream f(package_file(name), std::ios::binary);
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string iso_time(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    SYSTEMTIME t{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || !FileTimeToSystemTime(&a.ftLastWriteTime, &t))
        return "";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond);
    return buf;
}

// <data>\installed_apps.json, upstream's registry (launcher/InstallCore.swift
// RegistryStore, schema 2) as MacApkPackageManager.InstalledRegistry reads
// it: one row per library app whose package is known. Rewritten whole, to a
// temporary file then renamed, so a reader sees an old or a new file.
void write_registry() {
    std::string out = "{\"schema\":2,\"apps\":[";
    bool first = true;
    std::vector<std::string> seen;  // one row per package, as on a device
    for (const App& app : library()) {
        const std::vector<std::string> p = package_of(app.name);
        if (p.empty() || p[0].empty()) continue;
        if (std::find(seen.begin(), seen.end(), p[0]) != seen.end()) continue;
        seen.push_back(p[0]);
        const std::string when = iso_time(package_file(app.name));
        out += first ? "\n" : ",\n";
        first = false;
        out += "{\"package\":" + json_string(p[0]) + ",\"label\":" + json_string(wide_to_utf8(app.label)) +
               ",\"versionCode\":" + std::to_string(p.size() > 1 ? std::atoll(p[1].c_str()) : 0) +
               ",\"versionName\":" + json_string(p.size() > 2 ? p[2] : "") +
               ",\"apkPath\":" + json_string(wide_to_utf8(app.apk.wstring())) +
               ",\"bundlePath\":" + json_string(wide_to_utf8(app.apk.wstring())) +
               ",\"dirName\":" + json_string(wide_to_utf8(app.name)) + ",\"state\":\"installed\"" +
               ",\"installedAt\":" + json_string(when) + ",\"updatedAt\":" + json_string(when) + "}";
    }
    out += "\n]}\n";
    std::error_code ec;
    fs::create_directories(data_dir(), ec);
    const fs::path file = data_dir() / L"installed_apps.json";
    const fs::path tmp = data_dir() / L"installed_apps.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << out;
        if (!f) return;
    }
    MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Apps installed before the library recorded packages get theirs read now,
// in the background, so the registry knows them too. (Only the name: the
// version comes with the app's next install.)
DWORD WINAPI backfill_thread(void*) {
    bool changed = false;
    std::error_code ec;
    for (const App& app : library()) {
        if (fs::exists(package_file(app.name), ec)) continue;
        HANDLE p = start_runtime(L"--apk \"" + app.apk.wstring() + L"\" --package-to \"" +
                                     package_file(app.name).wstring() + L"\"",
                                 logs_dir() / L"package-backfill.log");
        if (p && wait_exit(p) == 0) changed = true;
        else fs::remove(package_file(app.name), ec);
    }
    if (changed) {
        write_registry();
        if (g_main) PostMessageW(g_main, kRefresh, 0, 0);  // the store sees these packages as installed
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The install helper

// The helper's output goes where its caller pointed it (a transcript file
// PackageInstallerBackend reads), in UTF-8.
void helper_say(const std::string& line) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) return;
    const std::string s = line + "\n";
    DWORD n = 0;
    WriteFile(out, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
}

void helper_progress(double fraction, const char* stage) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "[helper] progress %.2f %s", fraction, stage);
    helper_say(buf);
}

int helper_fail(int code, const char* stage, const std::string& message) {
    helper_say("{\"ok\":false,\"stage\":" + json_string(stage) + ",\"message\":" + json_string(message) + "}");
    return code;
}

// The library app that holds `pkg`, or null.
const App* find_package(const std::vector<App>& apps, const std::string& pkg) {
    for (const App& app : apps) {
        const std::vector<std::string> p = package_of(app.name);
        if (!p.empty() && p[0] == pkg) return &app;
    }
    return nullptr;
}

// Asks the user, as the Mac's helper does before it acts for an app. A no is
// exit 3, which the installer reports as STATUS_FAILURE_ABORTED.
bool helper_consent(const std::wstring& text) {
    const UINT rtl = g_rtl ? MB_RTLREADING | MB_RIGHT : 0;
    return MessageBoxW(nullptr, text.c_str(), L"Win.apk",
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND | rtl) == IDYES;
}

void notify_library() {
    if (HWND w = FindWindowW(L"WinApkLibrary", nullptr)) PostMessageW(w, kRefresh, 0, 0);
}

int helper_main(int argc, wchar_t** argv) {
    const std::wstring verb = argv[1];
    std::wstring apk, package, source, requestor;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&](std::wstring* out) {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        bool ok = true;
        if (a == L"--apk") ok = value(&apk);
        else if (a == L"--package") ok = value(&package);
        else if (a == L"--source") ok = value(&source);
        else if (a == L"--requestor-label") ok = value(&requestor);
        else if (a == L"--json") {}
        else ok = false;
        if (!ok) return helper_fail(2, "args", "unknown argument " + wide_to_utf8(a));
    }
    if (source != L"android") return helper_fail(2, "args", "--source android is required");
    const std::wstring who = requestor.empty() ? tr(L"An app") : requestor;
    const std::vector<App> apps = library();

    if (verb == L"uninstall") {
        const App* app = package.empty() ? nullptr : find_package(apps, wide_to_utf8(package));
        if (!app) return helper_fail(5, "lookup", "not installed: " + wide_to_utf8(package));
        if (!helper_consent(fmt(L"{0} wants to remove {1}.\n\nRemove it?", {who, app->label})))
            return helper_fail(3, "consent", "declined");
        remove_app(*app, false);
        notify_library();
        helper_say("{\"ok\":true,\"package\":" + json_string(wide_to_utf8(package)) + ",\"dataRemoved\":\"false\"}");
        return 0;
    }

    std::error_code ec;
    if (apk.empty() || !fs::is_regular_file(apk, ec)) return helper_fail(2, "args", "no --apk file");
    if (!is_app_file(apk)) return helper_fail(6, "invalid-apk", "not an .apk/.apkm/.xapk/.apks");

    // Which app this is decides what it replaces, so it is read first.
    const fs::path pkg_out = data_dir() / L"staging" / (L"package-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    fs::create_directories(pkg_out.parent_path(), ec);
    HANDLE probe = start_runtime(L"--apk \"" + apk + L"\" --package-to \"" + pkg_out.wstring() + L"\"",
                                 logs_dir() / L"install-helper.log");
    std::string pkg;
    if (probe && wait_exit(probe) == 0) {
        std::ifstream f(pkg_out, std::ios::binary);
        std::getline(f, pkg);
    }
    fs::remove(pkg_out, ec);
    if (pkg.empty()) return helper_fail(6, "invalid-apk", "no package name in the app");
    helper_progress(0.05, "parsed");

    const App* existing = find_package(apps, pkg);
    const bool update = existing != nullptr;
    const std::wstring wpkg = utf8_to_wide(pkg);
    const std::wstring question =
        update ? fmt(L"{0} wants to update {1} ({2}).\n\nUpdate it?", {who, existing->label, wpkg})
               : fmt(L"{0} wants to install {1}.\n\nInstall it?", {who, wpkg});
    if (!helper_consent(question)) return helper_fail(3, "consent", "declined");
    helper_progress(0.1, "consented");

    const std::wstring name = update ? existing->name : file_name_safe(wpkg);
    bool running = false;
    if (!install_into_library(apk, name, update ? existing->apk : fs::path(), &running)) {
        if (running) return helper_fail(5, "install", "the app is running; close it, then update it");
        return helper_fail(5, "install", "installing failed; the log is " +
                                             wide_to_utf8(install_log(name).wstring()));
    }
    helper_progress(1.0, "installed");
    notify_library();
    helper_say("{\"ok\":true,\"package\":" + json_string(pkg) + ",\"wasUpdate\":\"" + (update ? "true" : "false") +
               "\"}");
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // A shortcut: run the app, and only show anything if it fails.
    if (argc >= 3 && wcscmp(argv[1], L"--run") == 0) {
        pick_language();
        const std::wstring label = label_of(fs::path(argv[2]).stem().wstring());
        if (!run_and_wait(argv[2])) {
            failed_with_log(fmt(L"{0} stopped with an error", {label}), last_run_log(), true);
            return 1;
        }
        // run_and_wait returning true means the runtime exited normally. A missing
        // window marker after shutdown is not an error because the log may be flushed
        // after the HWND and Direct2D/DXGI resources have already been released.
        return 0;
    }

    // The install helper, for apps that install apps.
    if (argc >= 2 && (wcscmp(argv[1], L"install") == 0 || wcscmp(argv[1], L"uninstall") == 0)) {
        pick_language();
        const int code = helper_main(argc, argv);
        LocalFree(argv);
        OleUninitialize();
        return code;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);
    integrate_with_windows();
    write_registry();  // apps installed before the registry existed, or by an older launcher
    if (HANDLE t = CreateThread(nullptr, 0, backfill_thread, nullptr, 0, nullptr)) CloseHandle(t);
    pick_language();
    pick_theme();
    theme_menus();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WinApkLibrary";
    RegisterClassExW(&wc);
    WNDCLASSEXW sc{sizeof(sc)};
    sc.lpfnWndProc = sheet_proc;
    sc.hInstance = inst;
    sc.hIcon = wc.hIcon;
    sc.hCursor = wc.hCursor;
    sc.lpszClassName = L"WinApkSheet";
    RegisterClassExW(&sc);
    sc.lpfnWndProc = install_proc;
    sc.lpszClassName = L"WinApkInstall";
    RegisterClassExW(&sc);
    sc.lpfnWndProc = log_proc;
    sc.lpszClassName = L"WinApkLogs";
    RegisterClassExW(&sc);
    const UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(layout_style(), wc.lpszClassName, L"Win.apk", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(980, static_cast<int>(dpi), 96),
                                MulDiv(720, static_cast<int>(dpi), 96), nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, show);

    // An .apk opened from Explorer, or dropped on the program: install and run.
    for (int i = 1; i < argc; ++i) {
        if (is_app_file(argv[i])) install(argv[i], true);
    }
    LocalFree(argv);
    if (g_apps.empty() && g_pending.empty()) set_view(kStoreView);  // nothing installed yet: where apps come from

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_icons.clear();
    g_f = Fonts{};
    Gdiplus::GdiplusShutdown(gdiplus);
    OleUninitialize();
    return 0;
}
