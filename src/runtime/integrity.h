// Code-patch ledger for local validation and rollback.
//
// The protection library hashes libil2cpp.so and libcovault-appsec.so sections.
// This ledger records every byte changed by our hooks, checks that the original
// section matches the installed APK profile, and can restore our patches on
// failure. This local check does not alter what an independent protection check
// sees; passing it is not evidence that AppSealing accepts the change.
//
// Rule from 12-kernelsu-appsealing.md: never fabricate the expected value, never
// adopt the live value as the expectation, and fail closed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <pthread.h>

#include <vector>

#include "runtime/owned_code.h"

namespace bybus::runtime {

uint32_t crc32_bytes(const uint8_t *bytes, size_t size);

// Width written by And64InlineHook for one target/replacement pair.
size_t hook_width(uintptr_t target, uintptr_t replacement);

struct CodePatch {
    uintptr_t module_base = 0;
    uintptr_t offset = 0;  // relative to module_base
    size_t size = 0;
    uint8_t before[24] = {};
    uint8_t after[24] = {};
    bool committed = false;
    // The protection library rewrites its transient 50048 instruction, so that
    // site is restored but never substituted in an integrity comparison.
    bool substitute = true;
};

class PatchLedger {
public:
    PatchLedger() = default;
    ~PatchLedger();
    PatchLedger(const PatchLedger &) = delete;
    PatchLedger &operator=(const PatchLedger &) = delete;

    // Captures the bytes that a hook is about to overwrite.
    bool record(uintptr_t module_base, uintptr_t address, size_t size, bool substitute = true);
    // Captures the bytes actually written (call after the hook is installed).
    void commit(uintptr_t address, size_t size);
    // Writes every recorded `before` byte back and mprotects the pages again.
    bool restore_all();
    // Restores and forgets only the patches belonging to one module.
    bool restore_module(uintptr_t module_base);

    size_t count() const;
    size_t substitute_count(uintptr_t module_base) const;
    // CRC of [module_base + offset, + size) with recorded original bytes
    // substituted. `owned` reports how many registered patches were counted.
    uint32_t section_crc(uintptr_t module_base, uintptr_t offset, size_t size,
                         size_t *owned) const;
    bool section_crc_checked(uintptr_t module_base, uintptr_t offset, size_t size,
                             uint32_t *crc, size_t *owned) const;
    // Byte-range CRC described by the protection library. Only `substitute`
    // patches participate; `substituted` counts substituted bytes.
    bool ranges_crc(uintptr_t module_base, uintptr_t module_size,
                    const std::vector<owned_code::Range> &ranges, uint32_t *crc,
                    size_t *substituted) const;
    // FNV-1a fingerprint of the range descriptor, matching the family template.
    static uint64_t range_fingerprint(const std::vector<owned_code::Range> &ranges);

private:
    std::vector<owned_code::Patch> patch_view(uintptr_t module_base) const;
    mutable pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::vector<CodePatch> patches_;
};

// Profile produced locally by bybusd from the installed APK; an empty profile
// means the build is unverified and hooks must not be installed.
struct IntegrityProfile {
    bool present = false;
    uint32_t text_crc = 0;
    uintptr_t text_offset = 0;  // legacy file offset
    size_t text_size = 0;
    uintptr_t text_vaddr = 0;   // module-relative address when available
    uintptr_t managed_vaddr = 0;
    size_t managed_size = 0;
    uint32_t managed_crc = 0;
};

// Validates that the original bytes recorded in the ledger match the APK
// profile. This is not a protection bypass or a device acceptance result.
bool verify_section(uintptr_t module_base, const IntegrityProfile &profile, PatchLedger *ledger,
                    uint32_t *live_crc);
bool verify_managed_section(uintptr_t module_base, const IntegrityProfile &profile,
                            PatchLedger *ledger, uint32_t *live_crc);

// Same accounting as the reference owned_changes_only_locked(): both sections
// share one `owned` accumulator and the total must cover every registered
// substitute patch. Callers treat the result as an audit record, not as a gate:
// LimbusCodeIntegrityCompat.cpp records the verdict and keeps running.
bool verify_owned_sections(uintptr_t module_base, const IntegrityProfile &profile,
                           PatchLedger *ledger, uint32_t *text_crc, uint32_t *managed_crc,
                           size_t *owned);

} // namespace bybus::runtime
