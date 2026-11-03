# Native audio jint build fix

`JNIEnv` defines `jint` as a platform JNI integer type. The seek clamp now passes `jint{0}` to `std::max`, so both arguments have the same type under MSYS2 UCRT64.
