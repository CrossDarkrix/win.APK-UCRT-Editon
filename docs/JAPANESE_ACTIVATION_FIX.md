# Japanese activation fix

Japanese translation lookup now uses the persisted `Language` DWORD as the authoritative fallback, matching the value used by the Settings choice row. After a language selection is saved, the launcher reloads that exact value before rebuilding all visible UI. Hebrew behavior and RTL layout remain unchanged.
