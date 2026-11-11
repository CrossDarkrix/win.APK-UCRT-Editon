# Phase 20-B2.3.6.4.2 Command-Line Compile Fix

- Replaces the malformed one-line wide-string command construction in stdio.cpp.
- Builds the CreateProcessW command line with push_back and append operations.
- Preserves quoted executable and mapping-name arguments.
- Renames the readiness-loop variable to wait to avoid reuse of the path-position variable.
