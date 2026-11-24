#include "runtime/state.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/appsealing.h"

namespace bybus::runtime {
namespace {

std::atomic<bool> g_receipt_running{false};
std::atomic<int64_t> g_started_millis{0};

int64_t now_millis() {
    struct timespec now {};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

int64_t started_millis() {
    int64_t started = g_started_millis.load();
    if (started == 0) {
        const int64_t candidate = now_millis();
        if (g_started_millis.compare_exchange_strong(started, candidate)) return candidate;
    }
    return started;
}

std::string receipt_json(const char *hook_state, const char *font_state, const char *code) {
    RuntimeState &runtime = state();
    const int64_t survived = now_millis() - started_millis();
    // A launch only counts as confirmed after it survived long enough for the
    // protection checks to have run with the hooks in place.
    const bool confirmed = runtime.hooks_installed.load() && survived >= 15000;
    const CompatState compat = compat_state();
    char buffer[768];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"schema\":1,\"generation\":\"%s\",\"hookState\":\"%s\","
                  "\"fontState\":\"%s\",\"compatState\":\"%s\",\"hits\":%llu,\"misses\":%llu,"
                  "\"code\":\"%s\",\"startedAtMillis\":%lld,\"survivedMillis\":%lld,"
                  "\"confirmed\":%s}",
                  runtime.reader.generation().c_str(), hook_state != nullptr ? hook_state : "none",
                  font_state != nullptr ? font_state : "none", compat_state_text(compat),
                  static_cast<unsigned long long>(runtime.hits.load()),
                  static_cast<unsigned long long>(runtime.misses.load()),
                  code != nullptr ? code : "ok",
                  static_cast<long long>(started_millis()), static_cast<long long>(survived),
                  confirmed ? "true" : "false");
    return std::string(buffer);
}

void *updater_main(void *) {
    int ticks = 0;
    while (g_receipt_running.load()) {
        for (int i = 0; i < 10 && g_receipt_running.load(); ++i) ::usleep(100 * 1000);
        if (!g_receipt_running.load()) break;
        maintain_appsealing_compat();
        if (++ticks < 5) continue;
        ticks = 0;
        RuntimeState &runtime = state();
        const char *font_code = runtime.font_ready.load()
                                    ? "ok"
                                    : runtime.font_component_rejected.load()
                                          ? "font_component_rejected"
                                          : "font_invalid";
        runtime.client.send_receipt(receipt_json(runtime.hooks_installed.load() ? "installed" : "none",
                                                 runtime.font_ready.load() ? "ready" : "none",
                                                 font_code));
    }
    return nullptr;
}

} // namespace

RuntimeState &state() {
    static RuntimeState runtime;
    return runtime;
}

void write_receipt(const char *hook_state, const char *font_state, const char *code) {
    state().client.send_receipt(receipt_json(hook_state, font_state, code));
}

void start_receipt_updater() {
    if (g_receipt_running.exchange(true)) return;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, updater_main, nullptr) != 0) {
        g_receipt_running.store(false);
        return;
    }
    pthread_detach(thread);
}

} // namespace bybus::runtime

