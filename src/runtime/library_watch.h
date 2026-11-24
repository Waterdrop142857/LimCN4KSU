// Loader notification for libil2cpp. The Android linker exposes
// android_dlopen_ext; hooking it avoids waking a monitor thread every 100 ms and
// lets a slow or delayed Unity start use the full deadline without polling.
#pragma once

#include <cstddef>

namespace bybus::runtime {

struct LibraryWatch;
LibraryWatch *start_library_watch(const char *soname);
bool wait_for_library(LibraryWatch *watch, int timeout_ms);
bool library_watch_hooked(LibraryWatch *watch);

} // namespace bybus::runtime
