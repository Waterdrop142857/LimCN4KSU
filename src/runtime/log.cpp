#include "runtime/log.h"

#include <android/log.h>

#include <cstdarg>
#include <cstdio>

namespace bybus::runtime {
namespace {

constexpr const char *kTag = "bybus";

} // namespace

Counters &counters() {
    static Counters value;
    return value;
}

void log_info(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    __android_log_vprint(ANDROID_LOG_INFO, kTag, format, arguments);
    va_end(arguments);
}

void log_warn(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    __android_log_vprint(ANDROID_LOG_WARN, kTag, format, arguments);
    va_end(arguments);
}

} // namespace bybus::runtime
