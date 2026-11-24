// Process-wide runtime state for the official game. Everything is created on
// demand inside the specialised process; no global constructors run in zygote.
#pragma once

#include <pthread.h>

#include <atomic>
#include <cstdint>

#include "runtime/companion.h"
#include "runtime/il2cpp.h"
#include "runtime/integrity.h"
#include "runtime/snapshot_reader.h"

namespace bybus::runtime {

struct RuntimeState {
    CompanionClient client;
    SnapshotReader reader;
    Il2CppApi api;
    PatchLedger ledger;
    IntegrityProfile profile;

    std::atomic<bool> snapshot_ready{false};
    std::atomic<bool> hooks_installed{false};
    // Verified on the monitor thread before hooks are installed.
    std::atomic<bool> font_digest_verified{false};
    std::atomic<bool> font_ready{false};
    // A verified font that the live TMP component refused to accept. Kept
    // separate from a missing/corrupt font so diagnostics name the real stage.
    std::atomic<bool> font_component_rejected{false};
    // Gate for replacements: false until the font is proven and hooks verified.
    std::atomic<bool> enabled{false};
    // Unity font and TMP APIs may only be called on the process main thread;
    // recorded in postAppSpecialize, before the monitor thread starts.
    pthread_t main_thread{};
    std::atomic<bool> main_thread_known{false};

    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> rejects{0};
};

RuntimeState &state();

// Persists the receipt through the companion. `code` is a protocol code text.
void write_receipt(const char *hook_state, const char *font_state, const char *code);

// True once the localization entry hooks (LoadLocalizeFileSync and
// TextAsset.get_text) own a trampoline. Optional hooks may fail individually;
// that is only logged, never reported through this predicate.
bool text_entry_installed();

// Starts the detached receipt updater (5 s cadence, stops with the process).
void start_receipt_updater();

} // namespace bybus::runtime
