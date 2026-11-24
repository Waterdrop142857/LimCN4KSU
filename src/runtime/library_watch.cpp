#include "runtime/library_watch.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <string>

#include "CydiaSubstrate.h"

namespace bybus::runtime {
namespace {
typedef void *(*android_dlopen_ext_fn)(const char *, int, const void *);
android_dlopen_ext_fn g_original_dlopen_ext = nullptr;
std::atomic<LibraryWatch *> g_active_watch{nullptr};

bool basename_matches(const char *path, const char *soname) {
    if (!path || !soname) return false;
    const char *base = ::strrchr(path, '/');
    base = base ? base + 1 : path;
    return ::strcmp(base, soname) == 0;
}
} // namespace

struct LibraryWatch {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    std::string soname;
    bool ready = false;
    bool hooked = false;
};

namespace {
void mark_ready(LibraryWatch *watch) {
    if (!watch) return;
    pthread_mutex_lock(&watch->mutex);
    watch->ready = true;
    pthread_cond_broadcast(&watch->condition);
    pthread_mutex_unlock(&watch->mutex);
}

void *replacement_dlopen_ext(const char *filename, int flags, const void *info) {
    android_dlopen_ext_fn original = g_original_dlopen_ext;
    // If the trampoline is missing, never turn the loader into a no-op: fall
    // back to the public dlopen so Unity can still load libil2cpp and friends.
    void *result = original ? original(filename, flags, info)
                            : (filename ? ::dlopen(filename, flags) : nullptr);
    LibraryWatch *watch = g_active_watch.load(std::memory_order_acquire);
    if (result && watch && basename_matches(filename, watch->soname.c_str()))
        mark_ready(watch);
    return result;
}
} // namespace

LibraryWatch *start_library_watch(const char *soname) {
    if (!soname || !*soname) return nullptr;
    auto *watch = new LibraryWatch();
    watch->soname = soname;
    // A library loaded before postAppSpecialize still needs no loader event.
    void *already = ::dlopen(soname, RTLD_NOW | RTLD_NOLOAD);
    if (already) {
        watch->ready = true;
        ::dlclose(already);
    }
    void *symbol = ::dlsym(RTLD_DEFAULT, "android_dlopen_ext");
    if (symbol) {
        g_active_watch.store(watch, std::memory_order_release);
        MSHookFunction(symbol, reinterpret_cast<void *>(replacement_dlopen_ext),
                       reinterpret_cast<void **>(&g_original_dlopen_ext));
        watch->hooked = g_original_dlopen_ext != nullptr;
        // Re-probe after installing the hook: a load that raced with setup
        // would otherwise be invisible until the fallback timeout.
        if (!watch->ready) {
            already = ::dlopen(soname, RTLD_NOW | RTLD_NOLOAD);
            if (already) {
                watch->ready = true;
                ::dlclose(already);
            }
        }
    } else {
        g_active_watch.store(watch, std::memory_order_release);
    }
    return watch;
}

bool wait_for_library(LibraryWatch *watch, int timeout_ms) {
    if (!watch) return false;
    pthread_mutex_lock(&watch->mutex);
    if (watch->ready || !watch->hooked) {
        const bool ready = watch->ready;
        pthread_mutex_unlock(&watch->mutex);
        if (ready) g_active_watch.store(nullptr, std::memory_order_release);
        return ready;
    }
    struct timespec deadline {};
    ::clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    while (!watch->ready) {
        struct timespec slice = deadline;
        struct timespec now {};
        ::clock_gettime(CLOCK_REALTIME, &now);
        const int64_t remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                                     (deadline.tv_nsec - now.tv_nsec) / 1000000;
        if (remaining_ms <= 0) break;
        const int64_t slice_ms = std::min<int64_t>(remaining_ms, 100);
        slice.tv_sec = now.tv_sec + slice_ms / 1000;
        slice.tv_nsec = now.tv_nsec + (slice_ms % 1000) * 1000000L;
        if (slice.tv_nsec >= 1000000000L) {
            ++slice.tv_sec;
            slice.tv_nsec -= 1000000000L;
        }
        const int rc = pthread_cond_timedwait(&watch->condition, &watch->mutex, &slice);
        if (rc != 0 && rc != ETIMEDOUT) break;
        if (watch->ready) break;
        // bionic may load a library through an internal path that never calls
        // the public android_dlopen_ext symbol. Re-probe between waits so that
        // case is still detected without a full 90 s sleep.
        pthread_mutex_unlock(&watch->mutex);
        void *loaded = ::dlopen(watch->soname.c_str(), RTLD_NOW | RTLD_NOLOAD);
        pthread_mutex_lock(&watch->mutex);
        if (loaded) {
            watch->ready = true;
            ::dlclose(loaded);
            break;
        }
    }
    const bool ready = watch->ready;
    pthread_mutex_unlock(&watch->mutex);
    if (ready) g_active_watch.store(nullptr, std::memory_order_release);
    return ready;
}

bool library_watch_hooked(LibraryWatch *watch) {
    return watch && watch->hooked;
}
} // namespace bybus::runtime
