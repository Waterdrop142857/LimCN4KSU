// Pure 471 AppSealing unwind-recovery policy. Kept free of signal headers so
// host tests can exercise every condition without raising SIGSEGV.
#pragma once

#include <cstdint>

#include "translation/snapshot.h"

namespace bybus::runtime {

struct SigsegvFrame {
    uintptr_t pc = 0;
    uintptr_t lr = 0;
    uintptr_t x0 = 0;
    uintptr_t x1 = 0;
    uint32_t minus_5 = 0;
    uint32_t minus_1 = 0;
    uint32_t at_0 = 0;
    uint32_t at_1 = 0;
    uint32_t minus_0x34 = 0;
};

bool should_recover_471(const translation::AppSealingSigsegv &plan, uintptr_t base,
                        const SigsegvFrame &frame, uintptr_t fault_address,
                        uintptr_t *recovery_pc);

inline bool should_recover_471(const translation::AppSealingSigsegv &plan, uintptr_t base,
                               const SigsegvFrame &frame, uintptr_t fault_address,
                               uintptr_t *recovery_pc) {
    if (!recovery_pc || !plan.present || plan.pattern.size() != 5 || base == 0 ||
        plan.pc_offset > UINTPTR_MAX - base ||
        plan.recovery_offset > UINTPTR_MAX - base) {
        return false;
    }
    const uintptr_t pc = base + static_cast<uintptr_t>(plan.pc_offset);
    if (frame.pc != pc || frame.lr > UINTPTR_MAX - 0x14 || frame.lr + 0x14 != pc ||
        frame.x1 != plan.x1_expected || fault_address != frame.x0 ||
        frame.minus_5 != plan.pattern[0] || frame.minus_1 != plan.pattern[1] ||
        frame.at_0 != plan.pattern[2] || frame.at_1 != plan.pattern[3] ||
        frame.minus_0x34 != plan.pattern[4]) {
        return false;
    }
    *recovery_pc = base + static_cast<uintptr_t>(plan.recovery_offset);
    return true;
}

} // namespace bybus::runtime
