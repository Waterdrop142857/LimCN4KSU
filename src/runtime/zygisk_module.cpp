// Zygisk entry point. It identifies the official game main process, takes the
// snapshot handles from the root companion, and installs the text hooks from a
// dedicated thread once IL2CPP is live. Nothing heavy runs in the zygote.
#include <jni.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include <cstdint>
#include <string>

#include "zygisk.hpp"

#include "runtime/appsealing.h"
#include "runtime/companion.h"
#include "runtime/hooks.h"
#include "runtime/il2cpp.h"
#include "runtime/log.h"
#include "runtime/state.h"

namespace {

constexpr const char *kTargetPackage = "com.ProjectMoon.LimbusCompany";
constexpr int kIl2CppPollAttempts = 900;      // 90 s at 100 ms
constexpr int kClassPollAttempts = 600;       // 60 s at 100 ms

void *monitor_main(void *) {
    bybus::runtime::RuntimeState &runtime = bybus::runtime::state();
    // AppSealing compatibility is installed before any text hook. A build that
    // declares the protection library but has no qualified plan stays unhooked.
    if (runtime.reader.profile().appsealing.present) {
        std::string error;
        if (!bybus::runtime::install_appsealing_compat(&error)) {
            bybus::runtime::log_warn("AppSealing compatibility failed: %s", error.c_str());
            bybus::runtime::write_receipt("rejected", "none", "unsupported_runtime");
            return nullptr;
        }
    } else if (!runtime.reader.profile().protection_sha256.empty()) {
        bybus::runtime::log_warn("no AppSealing plan for this build; hooks disabled");
        bybus::runtime::write_receipt("rejected", "none", "unsupported_runtime");
        return nullptr;
    }
    bool resolved = false;
    for (int attempt = 0; attempt < kIl2CppPollAttempts; ++attempt) {
        if (bybus::runtime::resolve_il2cpp(&runtime.api)) {
            resolved = true;
            break;
        }
        ::usleep(100 * 1000);
    }
    if (!resolved) {
        bybus::runtime::log_warn("libil2cpp.so unavailable");
        bybus::runtime::write_receipt("rejected", "none", "unsupported_runtime");
        return nullptr;
    }
    if (!runtime.profile.present) {
        // AppSealing layout unknown for this build: never patch blindly.
        bybus::runtime::log_warn("no integrity profile; hooks disabled");
        bybus::runtime::write_receipt("rejected", "none", "unsupported_runtime");
        return nullptr;
    }
    bybus::runtime::Il2CppThreadScope scope(runtime.api);
    bool classes_ready = false;
    for (int attempt = 0; attempt < kClassPollAttempts; ++attempt) {
        if (bybus::runtime::find_class(runtime.api, "TextAssetPatch", "TextAssetPatchManager") &&
            bybus::runtime::find_class(runtime.api, "UnityEngine", "TextAsset")) {
            classes_ready = true;
            break;
        }
        ::usleep(100 * 1000);
    }
    if (!classes_ready) {
        bybus::runtime::log_warn("localization classes never appeared");
        bybus::runtime::write_receipt("rejected", "none", "unsupported_runtime");
        return nullptr;
    }
    // The font descriptor is immutable for this generation. Hash it before
    // installing text hooks so Unity callbacks never read 24 MB of font data.
    if (!bybus::runtime::verify_font_snapshot()) {
        bybus::runtime::log_warn("font unavailable or digest invalid; hooks skipped");
        bybus::runtime::write_receipt("rejected", "rejected", "font_invalid");
        return nullptr;
    }
    // Verify and read published tables off the Unity callback thread. A table
    // that cannot be prewarmed still uses the checked on-demand lookup path.
    runtime.reader.prewarm_tables();
    if (!bybus::runtime::install_text_hooks()) {
        bybus::runtime::write_receipt("rejected", "none", "unsupported_runtime");
        return nullptr;
    }
    runtime.hooks_installed.store(true);
    // The first text call on Unity's main thread can initialise the TMP font.
    // Until that succeeds, all hooks return the game's original strings.
    runtime.enabled.store(false);
    bybus::runtime::write_receipt("installed", "none", "ok");
    bybus::runtime::start_receipt_updater();
    return nullptr;
}

class BybusModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
        if (env_) {
            JavaVM *vm = nullptr;
            if (env_->GetJavaVM(&vm) == JNI_OK && vm) {
                bybus::runtime::set_java_vm(vm);
            }
        }
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (!api_ || !env_ || !args) return;
        bool matches = false;
        if (args->nice_name) {
            const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);
            if (name) {
                matches = ::strcmp(name, kTargetPackage) == 0;
                env_->ReleaseStringUTFChars(args->nice_name, name);
            }
        }
        // The UID must be a real application uid; zygote and system are excluded.
        if (args->uid < 10000) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        if (!matches) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        companion_fd_ = api_->connectCompanion();
        if (companion_fd_ < 0) {
            bybus::runtime::log_warn("companion unavailable");
            target_ = false;
            return;
        }
        api_->exemptFd(companion_fd_);
        target_ = true;
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target_ || companion_fd_ < 0) return;
        bybus::runtime::RuntimeState &runtime = bybus::runtime::state();
        if (!runtime.client.connect(companion_fd_)) return;
        bybus::runtime::HelloInfo hello;
        if (!runtime.client.hello(&hello)) {
            bybus::runtime::log_warn("snapshot handles unavailable");
            bybus::runtime::write_receipt("rejected", "none", "io_error");
            return;
        }
        if (hello.degraded) {
            // An explicit manual marker disables this generation's hooks.
            bybus::runtime::log_warn("generation degraded, hooks skipped");
            if (hello.manifest_fd >= 0) ::close(hello.manifest_fd);
            if (hello.font_fd >= 0) ::close(hello.font_fd);
            bybus::runtime::write_receipt("rejected", "none", "unsupported_runtime");
            return;
        }
        std::string error;
        if (!runtime.reader.open(&runtime.client, hello, &error)) {
            bybus::runtime::log_warn("snapshot rejected: %s", error.c_str());
            bybus::runtime::write_receipt("rejected", "none", "snapshot_invalid");
            return;
        }
        const bybus::translation::SnapshotProfile &profile = runtime.reader.profile();
        runtime.profile.present = profile.present;
        runtime.profile.text_offset = static_cast<uintptr_t>(profile.text_offset);
        runtime.profile.text_size = static_cast<size_t>(profile.text_size);
        runtime.profile.text_crc = profile.text_crc;
        runtime.profile.text_vaddr = static_cast<uintptr_t>(profile.text_vaddr);
        runtime.profile.managed_vaddr = static_cast<uintptr_t>(profile.managed_vaddr);
        runtime.profile.managed_size = static_cast<size_t>(profile.managed_size);
        runtime.profile.managed_crc = profile.managed_crc;
        runtime.snapshot_ready.store(true);
        // Zygisk runs postAppSpecialize on the thread that becomes the process
        // main thread; font and TMP work is only legal there.
        runtime.main_thread = pthread_self();
        runtime.main_thread_known.store(true);
        pthread_t thread;
        if (pthread_create(&thread, nullptr, monitor_main, nullptr) != 0) {
            bybus::runtime::log_warn("monitor thread failed");
            return;
        }
        pthread_detach(thread);
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false;
    int companion_fd_ = -1;
};

} // namespace

REGISTER_ZYGISK_MODULE(BybusModule)
REGISTER_ZYGISK_COMPANION(bybus::runtime::serve_companion)
