#include "runtime/sigsegv_guard.h"

#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>

#include <atomic>
#include <cstdint>

#include "runtime/appsealing.h"
#include "runtime/sigsegv_policy.h"
#include "runtime/state.h"

namespace bybus::runtime {
namespace {

struct SavedAction {
    struct sigaction action {};
    bool valid = false;
};

SavedAction g_previous[2];
std::atomic<int> g_previous_index{0};
std::atomic<bool> g_previous_saved{false};
pthread_mutex_t g_install_mutex = PTHREAD_MUTEX_INITIALIZER;

void default_terminate(int signal) {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)::sigaction(signal, &action, nullptr);
    syscall(__NR_tgkill, getpid(), static_cast<int>(syscall(__NR_gettid)), signal);
    _exit(128 + signal);
}

bool context_changed(const mcontext_t &before, const mcontext_t &after) {
    for (size_t i = 0; i < 31; ++i) {
        if (before.regs[i] != after.regs[i]) return true;
    }
    return before.sp != after.sp || before.pc != after.pc || before.pstate != after.pstate;
}

uint32_t read_instruction(uintptr_t address) {
    return *reinterpret_cast<const volatile uint32_t *>(address);
}

void sigsegv_handler(int signal, siginfo_t *info, void *context) {
    if (info && info->si_code <= 0) return;
    if (!context) default_terminate(signal);
    const translation::AppSealingSigsegv &plan =
        state().reader.profile().appsealing.sigsegv;
    const uintptr_t base = appsealing_protection_base();
    if (plan.present && base && context && info) {
        auto *ucontext = static_cast<ucontext_t *>(context);
        mcontext_t &machine = ucontext->uc_mcontext;
        SigsegvFrame frame;
        frame.pc = static_cast<uintptr_t>(machine.pc);
        frame.lr = static_cast<uintptr_t>(machine.regs[30]);
        frame.x0 = static_cast<uintptr_t>(machine.regs[0]);
        frame.x1 = static_cast<uintptr_t>(machine.regs[1]);
        if (frame.pc >= base + 0x34) {
            const uintptr_t pc = frame.pc;
            frame.minus_5 = read_instruction(pc - 20);
            frame.minus_1 = read_instruction(pc - 4);
            frame.at_0 = read_instruction(pc);
            frame.at_1 = read_instruction(pc + 4);
            frame.minus_0x34 = read_instruction(pc - 0x34);
        }
        uintptr_t recovery = 0;
        if (should_recover_471(plan, base, frame,
                               reinterpret_cast<uintptr_t>(info->si_addr), &recovery)) {
            machine.pc = recovery;
            return;
        }
    }

    const int index = g_previous_index.load();
    if (!g_previous_saved.load()) default_terminate(signal);
    const SavedAction &saved = g_previous[index];
    if (!saved.valid) default_terminate(signal);
    const struct sigaction &previous = saved.action;
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction &&
        previous.sa_sigaction != sigsegv_handler) {
        const mcontext_t before = static_cast<ucontext_t *>(context)->uc_mcontext;
        previous.sa_sigaction(signal, info, context);
        if (context) {
            const auto *updated = static_cast<const ucontext_t *>(context);
            if (context_changed(before, updated->uc_mcontext)) return;
        }
        default_terminate(signal);
    }
    if (previous.sa_handler == SIG_IGN) return;
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_ERR &&
        previous.sa_handler != nullptr) {
        const mcontext_t before = static_cast<ucontext_t *>(context)->uc_mcontext;
        previous.sa_handler(signal);
        if (context) {
            const auto *updated = static_cast<const ucontext_t *>(context);
            if (context_changed(before, updated->uc_mcontext)) return;
        }
    }
    default_terminate(signal);
}

} // namespace

bool install_sigsegv_guard() {
    pthread_mutex_lock(&g_install_mutex);
    struct sigaction current {};
    if (::sigaction(SIGSEGV, nullptr, &current) != 0) {
        pthread_mutex_unlock(&g_install_mutex);
        return false;
    }
    if ((current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction == sigsegv_handler) {
        pthread_mutex_unlock(&g_install_mutex);
        return true;
    }
    const int next = g_previous_index.load() == 0 ? 1 : 0;
    g_previous[next].action = current;
    g_previous[next].valid = true;
    g_previous_saved.store(true);
    g_previous_index.store(next);
    struct sigaction guarded = current;
    guarded.sa_sigaction = sigsegv_handler;
    guarded.sa_flags |= SA_SIGINFO;
    const bool ok = ::sigaction(SIGSEGV, &guarded, nullptr) == 0;
    pthread_mutex_unlock(&g_install_mutex);
    return ok;
}

void maintain_sigsegv_guard() { (void)install_sigsegv_guard(); }

} // namespace bybus::runtime
