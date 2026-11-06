import os
adds = []
dels = []

def gg():
    for g in sorted(os.listdir('Win.apk-UCRT')):
        if os.path.isfile(os.path.join('Win.apk-UCRT', g)):
            adds.append('File "Win.apk-UCRT\\{}"'.format(g))
        else:
            adds.append('File /r "Win.apk-UCRT\\{}"'.format(g))
gg()
def extend():
    for r, d, f in os.walk('Win.apk-UCRT'):
        for ff in f:
            yield os.path.join(r, ff)

for g in extend():
    dels.append('Delete "$INSTDIR\\{}"'.format(g).replace('Win.apk-UCRT\\', ''))


data = '''!include MUI2.nsh
Name "Win.APK"
InstallDir "$PROGRAMFILES\\Win.apk-UCRT"
Outfile "Win.APK_Installer.exe"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "Japanese"
!define MUI_ABORTWARNING

Section
SetOutPath "$INSTDIR"
{}
WriteUninstaller "$INSTDIR\\Uninstall.exe"
CreateShortcut "$SMPROGRAMS\\Win.apk-UCRT.lnk" "$INSTDIR\\Win.apk.exe" ""
CreateShortcut "$DESKTOP\\Win.apk-UCRT.lnk" "$INSTDIR\\Win.apk.exe" ""
SectionEnd
Section "Uninstall"
{}
Delete "$INSTDIR\\Uninstall.exe"
RMDir /r "$INSTDIR"
Delete "$SMPROGRAMS\\Win.apk-UCRT.lnk"
Delete "$DESKTOP\\Win.apk-UCRT.lnk"
SectionEnd
'''.format('\n'.join(adds), '\n'.join(dels))

with open("Win.apk-UCRT.nsi", "w") as w:
    w.write(data)