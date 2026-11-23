// Reads the installed game APK and produces the AppSealing compatibility
// profile: the libil2cpp.so section layout the protection library hashes plus
// the verified libcovault-appsec.so compatibility plan.
//
// Only local, already-installed bytes are used. The profile binds the snapshot
// to one installed build; a different APK yields a different profile and the
// runtime refuses to patch.
#pragma once

#include <cstdint>
#include <string>

#include "translation/snapshot.h"

namespace bybus::translation {

struct IntegrityProfileData {
    bool present = false;
    std::string detail;
    std::string il2cpp_sha256;
    std::string protection_sha256;
    uint64_t text_offset = 0;
    uint64_t text_size = 0;
    uint32_t text_crc = 0;
    uint64_t text_vaddr = 0;
    uint64_t managed_vaddr = 0;
    uint64_t managed_size = 0;
    uint32_t managed_crc = 0;
    uint64_t rodata_offset = 0;
    uint64_t rodata_size = 0;
    uint32_t rodata_crc = 0;
    AppSealingPlan appsealing;
    std::string appsealing_detail;
};

// `apk_path` must be the installed base.apk. Reads lib/arm64-v8a/libil2cpp.so and
// lib/arm64-v8a/libcovault-appsec.so through the bounded archive reader.
// `version_code` is the installed package version used together with the
// structure fingerprints, never by itself, to select the 471 transient plan.
IntegrityProfileData collect_integrity_profile(const std::string &apk_path,
                                               int64_t version_code = 0);

} // namespace bybus::translation
