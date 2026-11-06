# Phase 19-A.1 UCRT64 compile fix

Fixes three clang++/MinGW UCRT64 issues in native_usb_direct.cpp: includes objbase.h for CLSIDFromString, replaces mixed jint/int std::max calls with an explicit positive-timeout conversion to ULONG, and fully zero-initializes SetupAPI structures before assigning cbSize. USB Direct Bridge behavior is otherwise unchanged.
