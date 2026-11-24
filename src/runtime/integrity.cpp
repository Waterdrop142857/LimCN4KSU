#include "runtime/integrity.h"

#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "runtime/log.h"

namespace bybus::runtime {
namespace {

constexpr size_t kMaxPatches = 512;

uintptr_t page_start(uintptr_t address) {
    const uintptr_t page = static_cast<uintptr_t>(::sysconf(_SC_PAGESIZE));
    return address & ~(page - 1);
}

bool set_writable(uintptr_t address, size_t size, bool writable) {
    const uintptr_t start = page_start(address);
    const uintptr_t end =
        page_start(address + size - 1) + static_cast<uintptr_t>(::sysconf(_SC_PAGESIZE));
    const int protection = writable ? (PROT_READ | PROT_WRITE | PROT_EXEC) : (PROT_READ | PROT_EXEC);
    return ::mprotect(reinterpret_cast<void *>(start), end - start, protection) == 0;
}

bool overlaps(const CodePatch &patch, uintptr_t offset, size_t size) {
    return patch.offset < offset + size && offset < patch.offset + patch.size;
}

// Reads only the mapped ELF header, never a loader enumeration from a hook.
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

uint32_t normalized_crc(const uint8_t *bytes, size_t size) {
    uint32_t crc = 0;
    size_t owned = 0;
    if (!owned_code::section_crc(bytes, 0, size, {}, crc, owned)) return 0;
    return crc;
}

struct SectionMapping {
    uintptr_t module_base = 0;
    uint64_t file_offset = 0;
    size_t size = 0;
    uintptr_t address = 0;
};

int find_text_mapping(struct dl_phdr_info *info, size_t, void *opaque) {
    auto *mapping = static_cast<SectionMapping *>(opaque);
    if (!info || !info->dlpi_name ||
        static_cast<uintptr_t>(info->dlpi_addr) != mapping->module_base) {
        return 0;
    }
    const char *name = std::strrchr(info->dlpi_name, '/');
    name = name ? name + 1 : info->dlpi_name;
    if (std::strcmp(name, "libil2cpp.so") != 0) return 0;
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_LOAD || (segment.p_flags & PF_X) == 0 ||
            mapping->file_offset < segment.p_offset) {
            continue;
        }
        const uint64_t relative = mapping->file_offset - segment.p_offset;
        if (relative > segment.p_filesz || mapping->size > segment.p_filesz - relative) continue;
        const uintptr_t segment_start =
            static_cast<uintptr_t>(info->dlpi_addr) + static_cast<uintptr_t>(segment.p_vaddr);
        if (segment_start < static_cast<uintptr_t>(info->dlpi_addr) ||
            relative > UINTPTR_MAX - segment_start) {
            continue;
        }
        mapping->address = segment_start + static_cast<uintptr_t>(relative);
        return 1;
    }
    return 0;
}

bool resolve_region(uintptr_t module_base, uintptr_t vaddr, uintptr_t file_offset, size_t size,
                    uintptr_t *address) {
    *address = 0;
    if (module_base == 0 || size == 0) return false;
    // New profiles carry the loaded address. The legacy file offset path stays
    // for already published generations.
    if (vaddr != 0) {
        if (!mapped_range(module_base, vaddr, size, PF_R | PF_X)) return false;
        *address = module_base + vaddr;
        return true;
    }
    SectionMapping mapping{module_base, file_offset, size, 0};
    ::dl_iterate_phdr(find_text_mapping, &mapping);
    *address = mapping.address;
    return mapping.address != 0;
}

} // namespace

uint32_t crc32_bytes(const uint8_t *bytes, size_t size) {
    uint32_t table[256];
    owned_code::build_crc_table(table);
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ bytes[i]) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

size_t hook_width(uintptr_t target, uintptr_t replacement) {
    const int64_t distance =
        (static_cast<int64_t>(replacement) - static_cast<int64_t>(target)) >> 2;
    const int64_t limit = 0x03ffffff >> 1;
    if (distance > -limit && distance < limit) return 4;
    return (target & 7u) ? 20 : 16;
}

PatchLedger::~PatchLedger() { pthread_mutex_destroy(&mutex_); }

bool PatchLedger::record(uintptr_t module_base, uintptr_t address, size_t size,
                         bool substitute) {
    if (module_base == 0 || address < module_base || size == 0 ||
        size > sizeof(CodePatch::before)) {
        return false;
    }
    const uintptr_t offset = address - module_base;
    if (offset > UINTPTR_MAX - size) return false;
    CodePatch patch;
    patch.module_base = module_base;
    patch.offset = offset;
    patch.size = size;
    patch.substitute = substitute;
    std::memcpy(patch.before, reinterpret_cast<const void *>(address), size);
    pthread_mutex_lock(&mutex_);
    for (const CodePatch &existing : patches_) {
        if (existing.module_base == module_base && overlaps(existing, offset, size)) {
            pthread_mutex_unlock(&mutex_);
            log_warn("patch ledger overlap");
            return false;
        }
    }
    if (patches_.size() >= kMaxPatches) {
        pthread_mutex_unlock(&mutex_);
        return false;
    }
    patches_.push_back(patch);
    pthread_mutex_unlock(&mutex_);
    return true;
}

void PatchLedger::commit(uintptr_t address, size_t size) {
    pthread_mutex_lock(&mutex_);
    for (CodePatch &patch : patches_) {
        if (patch.module_base + patch.offset != address || patch.size != size) continue;
        std::memcpy(patch.after, reinterpret_cast<const void *>(address), size);
        patch.committed = true;
        break;
    }
    pthread_mutex_unlock(&mutex_);
}

bool PatchLedger::restore_all() {
    bool restored = true;
    pthread_mutex_lock(&mutex_);
    for (const CodePatch &patch : patches_) {
        const uintptr_t address = patch.module_base + patch.offset;
        // A failed MSHookFunction can leave bytes changed without reaching
        // commit(); restore those too, but skip untouched pages.
        const bool changed = std::memcmp(reinterpret_cast<const void *>(address),
                                         patch.before, patch.size) != 0;
        if (!patch.committed && !changed) continue;
        if (!set_writable(address, patch.size, true)) {
            restored = false;
            continue;
        }
        std::memcpy(reinterpret_cast<void *>(address), patch.before, patch.size);
        if (!set_writable(address, patch.size, false)) restored = false;
        __builtin___clear_cache(reinterpret_cast<char *>(address),
                                reinterpret_cast<char *>(address + patch.size));
    }
    patches_.clear();
    pthread_mutex_unlock(&mutex_);
    return restored;
}

bool PatchLedger::restore_module(uintptr_t module_base) {
    bool restored = true;
    pthread_mutex_lock(&mutex_);
    for (auto it = patches_.begin(); it != patches_.end();) {
        if (it->module_base != module_base) {
            ++it;
            continue;
        }
        const uintptr_t address = it->module_base + it->offset;
        const bool changed =
            std::memcmp(reinterpret_cast<const void *>(address), it->before, it->size) != 0;
        if (it->committed || changed) {
            if (!set_writable(address, it->size, true)) {
                restored = false;
            } else {
                std::memcpy(reinterpret_cast<void *>(address), it->before, it->size);
                if (!set_writable(address, it->size, false)) restored = false;
                __builtin___clear_cache(reinterpret_cast<char *>(address),
                                        reinterpret_cast<char *>(address + it->size));
            }
        }
        it = patches_.erase(it);
    }
    pthread_mutex_unlock(&mutex_);
    return restored;
}

size_t PatchLedger::count() const {
    pthread_mutex_lock(&mutex_);
    const size_t value = patches_.size();
    pthread_mutex_unlock(&mutex_);
    return value;
}

size_t PatchLedger::substitute_count(uintptr_t module_base) const {
    pthread_mutex_lock(&mutex_);
    size_t value = 0;
    for (const CodePatch &patch : patches_) {
        if (patch.module_base == module_base && patch.substitute) ++value;
    }
    pthread_mutex_unlock(&mutex_);
    return value;
}

std::vector<owned_code::Patch> PatchLedger::patch_view(uintptr_t module_base) const {
    std::vector<owned_code::Patch> view;
    for (const CodePatch &patch : patches_) {
        if (!patch.committed || !patch.substitute || patch.module_base != module_base) continue;
        owned_code::Patch converted;
        converted.offset = patch.offset;
        converted.size = patch.size;
        converted.substitute = true;
        std::memcpy(converted.before, patch.before, patch.size);
        std::memcpy(converted.after, patch.after, patch.size);
        view.push_back(converted);
    }
    return view;
}

uint32_t PatchLedger::section_crc(uintptr_t module_base, uintptr_t offset, size_t size,
                                  size_t *owned) const {
    uint32_t crc = 0;
    (void)section_crc_checked(module_base, offset, size, &crc, owned);
    return crc;
}

bool PatchLedger::section_crc_checked(uintptr_t module_base, uintptr_t offset, size_t size,
                                      uint32_t *crc, size_t *owned) const {
    if (!crc || offset > UINTPTR_MAX - module_base || module_base == 0) return false;
    pthread_mutex_lock(&mutex_);
    const std::vector<owned_code::Patch> patches = patch_view(module_base);
    pthread_mutex_unlock(&mutex_);
    const auto *live = reinterpret_cast<const unsigned char *>(module_base + offset);
    size_t counted = 0;
    const bool ok = owned_code::section_crc(live, offset, size, patches, *crc, counted);
    if (owned) *owned = counted;
    return ok;
}

bool PatchLedger::ranges_crc(uintptr_t module_base, uintptr_t module_size,
                             const std::vector<owned_code::Range> &ranges, uint32_t *crc,
                             size_t *substituted) const {
    if (!crc || !substituted || module_base == 0) return false;
    pthread_mutex_lock(&mutex_);
    const std::vector<owned_code::Patch> patches = patch_view(module_base);
    pthread_mutex_unlock(&mutex_);
    const auto *live = reinterpret_cast<const unsigned char *>(module_base);
    return owned_code::ranges_crc(live, module_size, ranges, patches, *crc, *substituted);
}

uint64_t PatchLedger::range_fingerprint(const std::vector<owned_code::Range> &ranges) {
    return owned_code::range_fingerprint(ranges);
}

bool verify_section(uintptr_t module_base, const IntegrityProfile &profile, PatchLedger *ledger,
                    uint32_t *live_crc) {
    if (live_crc) *live_crc = 0;
    if (!profile.present || module_base == 0 || profile.text_size == 0) return false;
    uintptr_t start = 0;
    if (!resolve_region(module_base, profile.text_vaddr, profile.text_offset, profile.text_size,
                        &start)) {
        return false;
    }
    const auto *bytes = reinterpret_cast<const uint8_t *>(start);
    if (!ledger) {
        const uint32_t value = normalized_crc(bytes, profile.text_size);
        if (live_crc) *live_crc = value;
        return value == profile.text_crc;
    }
    // The ledger substitutes pre-patch bytes, so a complete and consistent
    // ledger reproduces the original digest. A mismatch means the patch record
    // or the profile is wrong, and the caller must roll back.
    uint32_t value = 0;
    size_t owned = 0;
    const bool checked = ledger->section_crc_checked(module_base, start - module_base,
                                                     profile.text_size, &value, &owned);
    if (live_crc) *live_crc = value;
    return checked && owned == ledger->substitute_count(module_base) &&
           value == profile.text_crc;
}

bool verify_managed_section(uintptr_t module_base, const IntegrityProfile &profile,
                            PatchLedger *ledger, uint32_t *live_crc) {
    if (live_crc) *live_crc = 0;
    if (!profile.present || module_base == 0 || profile.managed_size == 0 ||
        profile.managed_vaddr == 0) {
        return false;
    }
    if (!mapped_range(module_base, profile.managed_vaddr, profile.managed_size, PF_R | PF_X)) {
        return false;
    }
    const auto *bytes = reinterpret_cast<const uint8_t *>(module_base + profile.managed_vaddr);
    if (!ledger) {
        const uint32_t value = normalized_crc(bytes, profile.managed_size);
        if (live_crc) *live_crc = value;
        return value == profile.managed_crc;
    }
    uint32_t value = 0;
    size_t owned = 0;
    const bool checked = ledger->section_crc_checked(
        module_base, profile.managed_vaddr, profile.managed_size, &value, &owned);
    if (live_crc) *live_crc = value;
    return checked && value == profile.managed_crc;
}

bool verify_owned_sections(uintptr_t module_base, const IntegrityProfile &profile,
                           PatchLedger *ledger, uint32_t *text_crc, uint32_t *managed_crc,
                           size_t *owned) {
    if (text_crc) *text_crc = 0;
    if (managed_crc) *managed_crc = 0;
    if (owned) *owned = 0;
    if (!ledger || !profile.present || module_base == 0 || profile.text_size == 0 ||
        profile.managed_size == 0 || profile.managed_vaddr == 0) {
        return false;
    }
    uintptr_t text_start = 0;
    if (!resolve_region(module_base, profile.text_vaddr, profile.text_offset, profile.text_size,
                        &text_start)) {
        return false;
    }
    if (!mapped_range(module_base, profile.managed_vaddr, profile.managed_size, PF_R | PF_X)) {
        return false;
    }
    // The reference accounts both sections through one `owned` accumulator, so
    // a patch counted in either section is counted exactly once in the total.
    uint32_t text_value = 0;
    uint32_t managed_value = 0;
    size_t text_owned = 0;
    size_t managed_owned = 0;
    const bool text_ok = ledger->section_crc_checked(module_base, text_start - module_base,
                                                     profile.text_size, &text_value, &text_owned);
    const bool managed_ok = ledger->section_crc_checked(
        module_base, profile.managed_vaddr, profile.managed_size, &managed_value, &managed_owned);
    if (text_crc) *text_crc = text_value;
    if (managed_crc) *managed_crc = managed_value;
    const size_t accounted = text_owned + managed_owned;
    if (owned) *owned = accounted;
    return text_ok && managed_ok && accounted == ledger->substitute_count(module_base) &&
           text_value == profile.text_crc && managed_value == profile.managed_crc;
}

} // namespace bybus::runtime
