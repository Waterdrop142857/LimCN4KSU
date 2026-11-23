// Bounded logging for the Zygisk runtime. Only stages, counters and codes are
// written; never game text, translations, account data or device identifiers.
#pragma once

#include <cstdint>

namespace bybus::runtime {

void log_info(const char *format, ...) __attribute__((format(printf, 1, 2)));
void log_warn(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Counters exposed through the receipt written by the companion.
struct Counters {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t rejects = 0;
};

Counters &counters();

} // namespace bybus::runtime
