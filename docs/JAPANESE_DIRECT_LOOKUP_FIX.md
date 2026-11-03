# Japanese direct lookup fix

The Japanese std::map lookup was removed. All 140 literal tr() call-site strings are now matched by exact wcscmp branches generated from the existing Japanese translations. Missing exact call-site keys: 0. Language dispatch uses g_ui_language, and selection rebuilds both the sheet and main window synchronously.
