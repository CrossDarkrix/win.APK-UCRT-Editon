# Japanese live switch fix

Language selection now has an explicit in-process override which is set before persistence and before rebuilding the open settings sheet. Translation therefore changes immediately even if registry persistence is delayed or unavailable. Sheet hit-test targets are cleared after callback-driven rebuilds and the settings window is repainted synchronously.
