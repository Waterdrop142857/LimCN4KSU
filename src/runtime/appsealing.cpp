#include "runtime/appsealing.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "CydiaSubstrate.h"
#include "runtime/integrity.h"
#include "runtime/log.h"
#include "runtime/state.h"
#include "translation/snapshot.h"

namespace bybus::runtime {
namespace {

constexpr const char *kProtectionLibrary = "libcovault-appsec.so";
constexpr const char *kGameLibrary = "libil2cpp.so";
constexpr unsigned kReadFlag = PF_R;
constexpr unsigned kReadExecuteFlags = PF_R | PF_X;
constexpr int kLibraryPollAttempts = 3000;  // 30 s at 10 ms

struct DispatchResult {
    uintptr_t target;
    uintptr_t continuation;
};

translation::AppSealingPlan &plan_state() {
    // Function-local and intentionally leaked: no constructor runs in the
    // zygote, and the plan stays valid through process teardown.
    static translation::AppSealingPlan *plan = new translation::AppSealingPlan();
    return *plan;
}
uintptr_t g_protection_base = 0;
uint64_t g_ranges_fingerprint = 0;
std::atomic<CompatState> g_state{CompatState::None};
std::atomic<uint64_t> g_maintains{0};
std::atomic<JavaVM *> g_vm{nullptr};
pthread_mutex_t g_install_mutex = PTHREAD_MUTEX_INITIALIZER;

struct InstallLock {
    InstallLock() { pthread_mutex_lock(&g_install_mutex); }
    ~InstallLock() { pthread_mutex_unlock(&g_install_mutex); }
    InstallLock(const InstallLock &) = delete;
    InstallLock &operator=(const InstallLock &) = delete;
};

using Detach = void (*)();
Detach g_original_detach = nullptr;
void *g_original_report = nullptr;

struct SigillSlot {
    struct sigaction action {};
    bool valid = false;
};
SigillSlot g_sigill_previous[2];
std::atomic<int> g_sigill_index{0};
std::atomic<bool> g_sigill_saved{false};

extern "C" void appsealing_detach_entry();
extern "C" void appsealing_report_dispatch_entry();
extern "C" DispatchResult appsealing_detach_impl(uintptr_t record, uintptr_t caller,
                                                 uintptr_t caller_stack);
extern "C" DispatchResult appsealing_report_dispatch_impl(uintptr_t code, uintptr_t caller);

uintptr_t page_start(uintptr_t address) {
    const uintptr_t page = static_cast<uintptr_t>(::sysconf(_SC_PAGESIZE));
    return address & ~(page - 1);
}

bool set_writable(uintptr_t address, size_t size, bool writable) {
    const uintptr_t start = page_start(address);
    const uintptr_t end =
        page_start(address + size - 1) + static_cast<uintptr_t>(::sysconf(_SC_PAGESIZE));
    const int protection =
        writable ? (PROT_READ | PROT_WRITE | PROT_EXEC) : (PROT_READ | PROT_EXEC);
    return ::mprotect(reinterpret_cast<void *>(start), end - start, protection) == 0;
}

bool mapped_range(uintptr_t base, uintptr_t offset, size_t size, unsigned flags) {
    if (!base || offset > UINTPTR_MAX - base || size > UINTPTR_MAX - base - offset) return false;
    const auto *elf = reinterpret_cast<const ElfW(Ehdr) *>(base);
    if (std::memcmp(elf->e_ident, ELFMAG, SELFMAG) != 0 ||
        elf->e_ident[EI_CLASS] != ELFCLASS64 || elf->e_ident[EI_DATA] != ELFDATA2LSB ||
        elf->e_machine != EM_AARCH64 || elf->e_phentsize != sizeof(ElfW(Phdr)) ||
        elf->e_phnum == 0 || elf->e_phnum > 64 || elf->e_phoff > 4096 ||
        static_cast<uint64_t>(elf->e_phnum) * sizeof(ElfW(Phdr)) > 4096 - elf->e_phoff) {
        return false;
    }
    const auto *headers = reinterpret_cast<const ElfW(Phdr) *>(base + elf->e_phoff);
    for (unsigned i = 0; i < elf->e_phnum; ++i) {
        const ElfW(Phdr) &header = headers[i];
        if (header.p_type == PT_LOAD && (header.p_flags & flags) == flags &&
            offset >= header.p_vaddr &&
            offset - header.p_vaddr <= header.p_memsz &&
            size <= header.p_memsz - (offset - header.p_vaddr)) {
            return true;
        }
    }
    return false;
}

bool read_word(uintptr_t base, uint64_t vaddr, uint32_t *word) {
    if (!word || vaddr > UINTPTR_MAX - base ||
        !mapped_range(base, static_cast<uintptr_t>(vaddr), sizeof(uint32_t), kReadFlag)) {
        return false;
    }
    std::memcpy(word, reinterpret_cast<const void *>(base + static_cast<uintptr_t>(vaddr)),
                sizeof(uint32_t));
    return true;
}

struct ModuleQuery {
    const char *name;
    uintptr_t base;
};

int find_module_callback(struct dl_phdr_info *info, size_t, void *opaque) {
    auto *query = static_cast<ModuleQuery *>(opaque);
    if (!info || !info->dlpi_name || !query) return 0;
    const char *name = std::strrchr(info->dlpi_name, '/');
    name = name ? name + 1 : info->dlpi_name;
    if (std::strcmp(name, query->name) == 0) {
        query->base = static_cast<uintptr_t>(info->dlpi_addr);
        return 1;
    }
    return 0;
}

uintptr_t module_base(const char *name) {
    ModuleQuery query{name, 0};
    ::dl_iterate_phdr(find_module_callback, &query);
    return query.base;
}

bool matches_words(uintptr_t base, uint64_t vaddr, const std::vector<uint32_t> &expected) {
    if (expected.empty()) return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        uint32_t word = 0;
        if (!read_word(base, vaddr + i * 4, &word) || word != expected[i]) return false;
    }
    return true;
}

uint64_t parse_hex64(const std::string &text) {
    uint64_t value = 0;
    for (const char c : text) {
        uint64_t digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<uint64_t>(c - 'a' + 10);
        else return 0;
        value = (value << 4) | digit;
    }
    return value;
}

uint64_t fingerprint_ranges(const uint8_t *items, uint32_t count, uint32_t stride) {
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t *item = items + static_cast<size_t>(index) * stride;
        uint64_t offset = 0;
        uint32_t size = 0;
        std::memcpy(&offset, item, sizeof(offset));
        std::memcpy(&size, item + 8, sizeof(size));
        for (const uint64_t value : {offset, static_cast<uint64_t>(size)}) {
            for (unsigned shift = 0; shift < 64; shift += 8) {
                hash ^= (value >> shift) & 0xffu;
                hash *= 1099511628211ull;
            }
        }
    }
    return hash;
}

bool stack_has(const void *pointer, size_t minimum) {
    pthread_attr_t attr;
    void *stack = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return false;
    const int result = pthread_attr_getstack(&attr, &stack, &size);
    pthread_attr_destroy(&attr);
    if (result != 0) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(stack);
    const uintptr_t at = reinterpret_cast<uintptr_t>(pointer);
    return at >= begin && at <= begin + size && begin + size - at >= minimum;
}

bool has_current_jni_environment(uintptr_t caller_stack) {
    if (!plan_state().present || !stack_has(reinterpret_cast<const void *>(caller_stack),
                                      static_cast<size_t>(plan_state().stack.caller_stack_min_bytes))) {
        return false;
    }
    JavaVM *vm = g_vm.load();
    if (!vm) return false;
    JNIEnv *current = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&current), JNI_VERSION_1_6) != JNI_OK || !current) {
        return false;
    }
    const auto *env_slot = reinterpret_cast<const JNIEnv *const *>(
        caller_stack + plan_state().stack.env_offset);
    const auto *state_slot =
        reinterpret_cast<const uint32_t *>(caller_stack + plan_state().stack.state_offset);
    return *env_slot == current && *state_slot != 0;
}

bool read_stack_u32(uintptr_t caller_stack, uint64_t offset, uint32_t *value) {
    if (!value) return false;
    std::memcpy(value, reinterpret_cast<const void *>(caller_stack + offset), sizeof(*value));
    return true;
}

bool read_stack_ptr(uintptr_t caller_stack, uint64_t offset, uintptr_t *value) {
    if (!value) return false;
    std::memcpy(value, reinterpret_cast<const void *>(caller_stack + offset), sizeof(*value));
    return true;
}

bool is_confirmed_record(uintptr_t record, uint32_t *expected_text, uint32_t *expected_managed) {
    if (!plan_state().present ||
        !stack_has(reinterpret_cast<const void *>(record),
                   static_cast<size_t>(plan_state().stack.record_min_bytes))) {
        return false;
    }
    const char *name = reinterpret_cast<const char *>(record + plan_state().stack.name_offset);
    if (std::memcmp(name, kGameLibrary, sizeof("libil2cpp.so")) != 0) return false;
    uint64_t text_size = 0;
    uint64_t managed_size = 0;
    std::memcpy(&text_size,
                reinterpret_cast<const void *>(record + plan_state().stack.text_size_offset),
                sizeof(text_size));
    std::memcpy(&managed_size,
                reinterpret_cast<const void *>(record + plan_state().stack.managed_size_offset),
                sizeof(managed_size));
    if (text_size != plan_state().text_size || managed_size != plan_state().managed_size) return false;
    std::memcpy(expected_text,
                reinterpret_cast<const void *>(record + plan_state().stack.text_crc_offset),
                sizeof(*expected_text));
    std::memcpy(expected_managed,
                reinterpret_cast<const void *>(record + plan_state().stack.managed_crc_offset),
                sizeof(*expected_managed));
    // Do not adopt a live or caller-supplied expectation: the record must carry
    // the same original section digests that bybusd computed from the APK.
    return *expected_text == plan_state().text_crc && *expected_managed == plan_state().managed_crc;
}

bool owned_changes_only(uint32_t expected_text, uint32_t expected_managed) {
    RuntimeState &runtime = state();
    const uintptr_t module = reinterpret_cast<uintptr_t>(runtime.api.base);
    if (!module || !plan_state().present) return false;
    uint32_t text_crc = 0;
    uint32_t managed_crc = 0;
    size_t text_patches = 0;
    size_t managed_patches = 0;
    const bool text_ok = runtime.ledger.section_crc_checked(
        module, static_cast<uintptr_t>(plan_state().text_vaddr),
        static_cast<size_t>(plan_state().text_size), &text_crc, &text_patches);
    const bool managed_ok = runtime.ledger.section_crc_checked(
        module, static_cast<uintptr_t>(plan_state().managed_vaddr),
        static_cast<size_t>(plan_state().managed_size), &managed_crc, &managed_patches);
    const bool complete =
        text_patches + managed_patches == runtime.ledger.substitute_count(module);
    return text_ok && managed_ok && complete && text_crc == expected_text &&
           managed_crc == expected_managed;
}

void install_sigill_guard();
void maintain_sigill_guard();

void sigill_guard(int signal, siginfo_t *info, void *context) {
#if defined(__aarch64__)
    if (context && info && info->si_code == ILL_ILLOPC && g_state.load() == CompatState::Installed &&
        plan_state().present && plan_state().transient.present) {
        auto *ucontext = static_cast<ucontext_t *>(context);
        const uintptr_t pc = static_cast<uintptr_t>(ucontext->uc_mcontext.pc);
        const uintptr_t lr = static_cast<uintptr_t>(ucontext->uc_mcontext.regs[30]);
        if (pc == g_protection_base + plan_state().transient.sigill_pc_vaddr &&
            lr == g_protection_base + plan_state().transient.sigill_lr_vaddr) {
            ucontext->uc_mcontext.pc =
                g_protection_base + plan_state().transient.target_vaddr;
            return;
        }
    }
#endif
    const int index = g_sigill_index.load();
    if (g_sigill_saved.load()) {
        const SigillSlot &slot = g_sigill_previous[index];
        if (slot.valid) {
            const struct sigaction &action = slot.action;
            if ((action.sa_flags & SA_SIGINFO) != 0 && action.sa_sigaction &&
                action.sa_sigaction != sigill_guard) {
                action.sa_sigaction(signal, info, context);
                return;
            }
            if (action.sa_handler == SIG_IGN) return;
            if (action.sa_handler != SIG_DFL && action.sa_handler != SIG_ERR &&
                action.sa_handler != nullptr) {
                action.sa_handler(signal);
                return;
            }
        }
    }
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    ::sigaction(SIGILL, &default_action, nullptr);
    syscall(__NR_tgkill, getpid(), syscall(__NR_gettid), SIGILL);
}

void install_sigill_guard() {
    if (!plan_state().present || !plan_state().transient.present) return;
    struct sigaction current {};
    if (::sigaction(SIGILL, nullptr, &current) != 0) return;
    if ((current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction == sigill_guard) return;
    const int next = g_sigill_index.load() == 0 ? 1 : 0;
    g_sigill_previous[next].action = current;
    g_sigill_previous[next].valid = true;
    g_sigill_saved.store(true);
    g_sigill_index.store(next);
    struct sigaction guarded = current;
    guarded.sa_sigaction = sigill_guard;
    guarded.sa_flags |= SA_SIGINFO;
    (void)::sigaction(SIGILL, &guarded, nullptr);
}

void maintain_sigill_guard() { install_sigill_guard(); }

bool verify_plan_runtime(uintptr_t base, std::string *error) {
    if (!plan_state().detach.present ||
        !mapped_range(base, static_cast<uintptr_t>(plan_state().detach.entry_vaddr), 16,
                      kReadExecuteFlags)) {
        *error = "appsealing_module_mapping";
        return false;
    }
    if (!mapped_range(base, static_cast<uintptr_t>(plan_state().ranges_vaddr),
                      static_cast<size_t>(plan_state().stack.descriptor_entries_offset) +
                          static_cast<size_t>(plan_state().ranges_count) *
                              plan_state().stack.descriptor_stride,
                      kReadFlag)) {
        *error = "appsealing_descriptor_mapping";
        return false;
    }
    const auto *descriptor = reinterpret_cast<const uint8_t *>(
        base + static_cast<uintptr_t>(plan_state().ranges_vaddr));
    uint32_t count = 0;
    std::memcpy(&count, descriptor + plan_state().stack.descriptor_count_offset, sizeof(count));
    if (count != plan_state().ranges_count) {
        *error = "appsealing_descriptor_shape";
        return false;
    }
    if (fingerprint_ranges(descriptor + plan_state().stack.descriptor_entries_offset, count,
                           plan_state().stack.descriptor_stride) != g_ranges_fingerprint) {
        *error = "appsealing_descriptor_fingerprint";
        return false;
    }
    for (const translation::AppSealingGuard &guard : plan_state().guards) {
        uint32_t word = 0;
        if (!read_word(base, guard.vaddr, &word) || word != guard.expected) {
            *error = "appsealing_guard_mismatch";
            return false;
        }
    }
    for (const translation::AppSealingSite &site : plan_state().sites) {
        if (site.kind == translation::AppSealingSite::Kind::Branch) {
            const int64_t delta = static_cast<int64_t>(site.target_vaddr) -
                                  static_cast<int64_t>(site.vaddr);
            if ((delta & 3) != 0 || delta < -0x08000000LL || delta >= 0x08000000LL ||
                site.word != (0x14000000u |
                              (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu))) {
                *error = "appsealing_site_shape";
                return false;
            }
        } else if (site.kind == translation::AppSealingSite::Kind::Nop &&
                   site.word != 0xd503201fu) {
            *error = "appsealing_site_shape";
            return false;
        }
        uint32_t word = 0;
        const bool transient_site =
            plan_state().transient.present && site.vaddr == plan_state().transient.vaddr;
        if (!read_word(base, site.vaddr, &word) || (!transient_site && word != site.expected)) {
            *error = "appsealing_site_mismatch";
            return false;
        }
    }
    if (plan_state().transient.present) {
        const int64_t delta = static_cast<int64_t>(plan_state().transient.target_vaddr) -
                              static_cast<int64_t>(plan_state().transient.vaddr);
        if ((delta & 3) != 0 || delta < -0x08000000LL || delta >= 0x08000000LL ||
            plan_state().transient.branch_word !=
                (0x14000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu))) {
            *error = "appsealing_site_shape";
            return false;
        }
    }
    if (!plan_state().detach.present ||
        !matches_words(base, plan_state().detach.entry_vaddr, plan_state().detach.expected)) {
        *error = "appsealing_detach_entry";
        return false;
    }
    if (plan_state().report.present &&
        (!matches_words(base, plan_state().report.entry_vaddr, plan_state().report.expected) ||
         !matches_words(base, plan_state().report.pattern_vaddr, plan_state().report.pattern))) {
        *error = "appsealing_report_entry";
        return false;
    }
    return true;
}

bool apply_patch(uintptr_t base, const translation::AppSealingSite &site, std::string *error) {
    const uintptr_t address = base + static_cast<uintptr_t>(site.vaddr);
    uint32_t live = 0;
    if (!read_word(base, site.vaddr, &live)) {
        *error = "appsealing_site_unreadable";
        return false;
    }
    if (live == site.word) {
        // Already equal to the replacement: only valid when the original word
        // was the same, so no unrecorded change exists.
        if (site.word == site.expected) return true;
        *error = "appsealing_site_preexisting";
        return false;
    }
    const bool transient = plan_state().transient.present && site.vaddr == plan_state().transient.vaddr;
    if (!transient && live != site.expected) {
        *error = "appsealing_site_changed";
        return false;
    }
    if (!set_writable(address, sizeof(uint32_t), true)) {
        *error = "appsealing_mprotect";
        return false;
    }
    RuntimeState &runtime = state();
    if (!runtime.ledger.record(base, address, sizeof(uint32_t), site.substitute)) {
        (void)set_writable(address, sizeof(uint32_t), false);
        *error = "appsealing_ledger";
        return false;
    }
    std::memcpy(reinterpret_cast<void *>(address), &site.word, sizeof(site.word));
    __builtin___clear_cache(reinterpret_cast<char *>(address),
                            reinterpret_cast<char *>(address + sizeof(site.word)));
    (void)set_writable(address, sizeof(uint32_t), false);
    runtime.ledger.commit(address, sizeof(uint32_t));
    return true;
}

bool install_report_bridge(uintptr_t base, std::string *error) {
    if (!plan_state().report.present) return true;
    const uintptr_t address = base + static_cast<uintptr_t>(plan_state().report.entry_vaddr);
    const size_t width =
        hook_width(address, reinterpret_cast<uintptr_t>(&appsealing_report_dispatch_entry));
    RuntimeState &runtime = state();
    if (!runtime.ledger.record(base, address, width)) {
        *error = "appsealing_ledger";
        return false;
    }
    MSHookFunction(reinterpret_cast<void *>(address),
                   reinterpret_cast<void *>(&appsealing_report_dispatch_entry),
                   &g_original_report);
    if (!g_original_report) {
        (void)runtime.ledger.restore_module(base);
        *error = "appsealing_report_hook";
        return false;
    }
    runtime.ledger.commit(address, width);
    return true;
}

bool install_detach_hook(uintptr_t base, std::string *error) {
    const uintptr_t address = base + static_cast<uintptr_t>(plan_state().detach.entry_vaddr);
    const size_t width =
        hook_width(address, reinterpret_cast<uintptr_t>(&appsealing_detach_entry));
    RuntimeState &runtime = state();
    if (!runtime.ledger.record(base, address, width)) {
        *error = "appsealing_ledger";
        return false;
    }
    MSHookFunction(reinterpret_cast<void *>(address),
                   reinterpret_cast<void *>(&appsealing_detach_entry),
                   reinterpret_cast<void **>(&g_original_detach));
    if (!g_original_detach) {
        (void)runtime.ledger.restore_module(base);
        *error = "appsealing_detach_hook";
        return false;
    }
    runtime.ledger.commit(address, width);
    return true;
}

} // namespace

extern "C" DispatchResult appsealing_detach_impl(uintptr_t record, uintptr_t caller,
                                                 uintptr_t caller_stack) {
    if (g_state.load() == CompatState::Installed && plan_state().present && g_protection_base) {
        const uintptr_t base = g_protection_base;
        if (caller == base + plan_state().detach_caller_vaddr &&
            has_current_jni_environment(caller_stack)) {
            uint32_t index = 0;
            uint32_t descriptor_index = 0;
            uintptr_t module = 0;
            uint32_t expected = 0;
            const bool fields =
                read_stack_u32(caller_stack, plan_state().stack.index_offset, &index) &&
                read_stack_u32(caller_stack, plan_state().stack.descriptor_index_offset,
                               &descriptor_index) &&
                read_stack_ptr(caller_stack, plan_state().stack.module_offset, &module) &&
                read_stack_u32(caller_stack, plan_state().stack.expected_offset, &expected);
            const auto *descriptor = reinterpret_cast<const uint8_t *>(
                base + static_cast<uintptr_t>(plan_state().ranges_vaddr));
            uint32_t count = 0;
            std::memcpy(&count, descriptor + plan_state().stack.descriptor_count_offset, sizeof(count));
            if (fields && index == 0 && descriptor_index == 1 && module == base &&
                count == plan_state().ranges_count) {
                std::vector<owned_code::Range> ranges;
                ranges.reserve(count);
                const uint8_t *entries =
                    descriptor + plan_state().stack.descriptor_entries_offset;
                for (uint32_t i = 0; i < count; ++i) {
                    const uint8_t *item = entries + plan_state().stack.descriptor_stride * i;
                    owned_code::Range range;
                    uint32_t size = 0;
                    std::memcpy(&range.offset, item, sizeof(range.offset));
                    std::memcpy(&size, item + 8, sizeof(size));
                    range.size = size;
                    ranges.push_back(range);
                }
                uint32_t crc = 0;
                size_t substituted = 0;
                const uint64_t shape = PatchLedger::range_fingerprint(ranges);
                const bool valid = shape == g_ranges_fingerprint &&
                                   state().ledger.ranges_crc(base, plan_state().module_size, ranges,
                                                             &crc, &substituted) &&
                                   substituted > 0 && crc == expected;
                if (valid) {
                    return {0, base + plan_state().detach_continuation_vaddr};
                }
            }
        }
        if (caller == base + plan_state().confirm_caller_vaddr &&
            has_current_jni_environment(caller_stack)) {
            uint32_t expected_text = 0;
            uint32_t expected_managed = 0;
            if (is_confirmed_record(record, &expected_text, &expected_managed) &&
                owned_changes_only(expected_text, expected_managed)) {
                return {0, base + plan_state().confirm_continuation_vaddr};
            }
        }
    }
    if (g_original_detach) g_original_detach();
    return {0, 0};
}

extern "C" DispatchResult appsealing_report_dispatch_impl(uintptr_t code, uintptr_t caller) {
    if (g_state.load() == CompatState::Installed && plan_state().present && plan_state().report.present &&
        g_protection_base && code == plan_state().report.code &&
        caller == g_protection_base + plan_state().report.caller_vaddr &&
        matches_words(g_protection_base, plan_state().report.pattern_vaddr, plan_state().report.pattern)) {
        return {0, g_protection_base + plan_state().report.continuation_vaddr};
    }
    return {reinterpret_cast<uintptr_t>(g_original_report), 0};
}

#if defined(__aarch64__)
extern "C" __attribute__((naked)) void appsealing_detach_entry() {
    __asm__ volatile(
        "mov x0, x19\n"
        "mov x1, x30\n"
        "mov x2, sp\n"
        "stp x29, x30, [sp, #-16]!\n"
        "mov x29, sp\n"
        "bl appsealing_detach_impl\n"
        "ldp x29, x30, [sp], #16\n"
        "cmp x1, #0\n"
        "csel x30, x30, x1, eq\n"
        "ret\n");
}

extern "C" __attribute__((naked)) void appsealing_report_dispatch_entry() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-224]!\n"
        "mov x29, sp\n"
        "stp x0, x1, [sp, #16]\n"
        "stp x2, x3, [sp, #32]\n"
        "stp x4, x5, [sp, #48]\n"
        "stp x6, x7, [sp, #64]\n"
        "stp x8, x9, [sp, #80]\n"
        "stp q0, q1, [sp, #96]\n"
        "stp q2, q3, [sp, #128]\n"
        "stp q4, q5, [sp, #160]\n"
        "stp q6, q7, [sp, #192]\n"
        "mov x0, x2\n"
        "mov x1, x30\n"
        "bl appsealing_report_dispatch_impl\n"
        "mov x16, x0\n"
        "mov x17, x1\n"
        "ldp x0, x1, [sp, #16]\n"
        "ldp x2, x3, [sp, #32]\n"
        "ldp x4, x5, [sp, #48]\n"
        "ldp x6, x7, [sp, #64]\n"
        "ldp x8, x9, [sp, #80]\n"
        "ldp q0, q1, [sp, #96]\n"
        "ldp q2, q3, [sp, #128]\n"
        "ldp q4, q5, [sp, #160]\n"
        "ldp q6, q7, [sp, #192]\n"
        "ldp x29, x30, [sp], #224\n"
        "cbnz x17, 1f\n"
        "br x16\n"
        "1: mov x30, x17\n"
        "ret\n");
}
#endif

CompatState compat_state() { return g_state.load(); }

const char *compat_state_text(CompatState state_value) {
    switch (state_value) {
        case CompatState::Installed:
            return "installed";
        case CompatState::Rejected:
            return "rejected";
        default:
            return "none";
    }
}

void set_java_vm(JavaVM *vm) { g_vm.store(vm); }

bool install_appsealing_compat(std::string *error) {
    if (!error) return false;
    InstallLock guard;
    const auto fail = [error](const char *code) {
        g_state.store(CompatState::Rejected);
        *error = code;
        return false;
    };
    const CompatState current = g_state.load();
    if (current == CompatState::Installed) return true;
    if (current == CompatState::Rejected) return fail("appsealing_rejected");
    const translation::AppSealingPlan &plan = state().reader.profile().appsealing;
    if (!plan.present) return fail("appsealing_plan_missing");
    plan_state() = plan;
    g_ranges_fingerprint = parse_hex64(plan_state().ranges_fingerprint);
    if (g_ranges_fingerprint == 0) {
        plan_state() = translation::AppSealingPlan();
        return fail("appsealing_fingerprint");
    }
    uintptr_t base = 0;
    for (int attempt = 0; attempt < kLibraryPollAttempts; ++attempt) {
        base = module_base(kProtectionLibrary);
        if (base) break;
        ::usleep(10 * 1000);
    }
    if (!base) {
        plan_state() = translation::AppSealingPlan();
        return fail("appsealing_library_missing");
    }
    g_protection_base = base;
    // The library can become visible in dl_iterate_phdr before its constructors
    // have populated the range descriptor. Retry the same bounded verification
    // window instead of locking in a permanent rejection on the first look.
    bool verified = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (verify_plan_runtime(base, error)) {
            verified = true;
            break;
        }
        ::usleep(10 * 1000);
    }
    if (!verified) {
        plan_state() = translation::AppSealingPlan();
        g_protection_base = 0;
        g_state.store(CompatState::Rejected);
        return false;
    }
    if (!install_detach_hook(base, error)) {
        (void)state().ledger.restore_module(base);
        g_original_detach = nullptr;
        g_protection_base = 0;
        return fail("appsealing_detach_hook");
    }
    if (!install_report_bridge(base, error)) {
        (void)state().ledger.restore_module(base);
        g_original_detach = nullptr;
        g_original_report = nullptr;
        g_protection_base = 0;
        return fail("appsealing_report_hook");
    }
    for (const translation::AppSealingSite &site : plan_state().sites) {
        if (!apply_patch(base, site, error)) {
            (void)state().ledger.restore_module(base);
            g_original_detach = nullptr;
            g_original_report = nullptr;
            g_protection_base = 0;
            return fail("appsealing_site");
        }
    }
    install_sigill_guard();
    g_state.store(CompatState::Installed);
    log_info("AppSealing compatibility installed");
    return true;
}

void maintain_appsealing_compat() {
    if (g_state.load() != CompatState::Installed || !plan_state().present || !plan_state().transient.present ||
        !g_protection_base) {
        return;
    }
    uint32_t word = 0;
    if (!read_word(g_protection_base, plan_state().transient.vaddr, &word)) return;
    if (word != plan_state().transient.branch_word) {
        const uintptr_t address =
            g_protection_base + static_cast<uintptr_t>(plan_state().transient.vaddr);
        if (set_writable(address, sizeof(uint32_t), true)) {
            std::memcpy(reinterpret_cast<void *>(address), &plan_state().transient.branch_word,
                        sizeof(plan_state().transient.branch_word));
            __builtin___clear_cache(reinterpret_cast<char *>(address),
                                    reinterpret_cast<char *>(address + sizeof(uint32_t)));
            (void)set_writable(address, sizeof(uint32_t), false);
            g_maintains.fetch_add(1);
        }
    }
    maintain_sigill_guard();
}

uint64_t compat_maintain_count() { return g_maintains.load(); }

} // namespace bybus::runtime

