#pragma once

#include <jni.h>

// False means skipped or failed; caller-owned exceptions are preserved.
bool clear_cache(JNIEnv *env);
