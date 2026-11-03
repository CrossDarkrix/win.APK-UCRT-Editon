# Phase 14 Get-StartApps process fix
- Uses powershell.exe -EncodedCommand with UTF-16LE Base64.
- Forces BOM-less UTF-8 stdout.
- Removes cmd.exe-only caret escaping.
- Separates stdout and stderr.
- Records CreateProcess, exit code, timeout, byte count, raw line count, parsed count and invalid count.
- Java logs source, exact AUMID, normalized-name and icon counts.
