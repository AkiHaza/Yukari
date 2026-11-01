#pragma once

// Binder-only host tests need Zygisk's JNI type declarations, not a JVM.
using jint = int;
using jlong = long long;
using jboolean = unsigned char;
struct JNIEnv;
struct _jobject;
using jobject = _jobject *;
using jstring = jobject;
using jintArray = jobject;
using jobjectArray = jobject;
struct JNINativeMethod {
    char *name;
    char *signature;
    void *fnPtr;
};
