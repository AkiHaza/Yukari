#include "logger.h"

#include <android/log.h>
#include <cstdarg>

namespace {
constexpr const char *kTag = "Yukari";
} // namespace

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, kTag, fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, kTag, fmt, ap);
    va_end(ap);
}
