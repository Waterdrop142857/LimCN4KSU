// AppSealing compatibility plan builder (bybusd side).
//
// Qualification is structural and local: it reads the installed APK's
// libcovault-appsec.so and libil2cpp.so, verifies the immutable code-region
// fingerprints of the known arm64 family, and only then emits the offsets,
// expected instruction words and stack layout that the runtime may use. An
// unknown or changed build yields no plan, so the runtime installs nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "translation/snapshot.h"

namespace bybus::translation {

struct AppSealingQualifyInput {
    std::string protection;  // lib/arm64-v8a/libcovault-appsec.so, empty when absent
    std::string il2cpp;      // lib/arm64-v8a/libil2cpp.so
    int64_t version_code = 0;
};

// Fills `plan` only when every structural check passes. Returns false with a
// fixed short reason (no APK content) otherwise. The two images are passed by
// reference: the game library is large and must not be copied again.
bool build_appsealing_plan(const std::string &protection, const std::string &il2cpp,
                           int64_t version_code, AppSealingPlan *plan, std::string *error);

// Test-visible pure helpers; they carry no Android dependency.
uint32_t appsealing_section_crc(const uint8_t *data, size_t size);
uint64_t appsealing_range_fingerprint(const uint8_t *items, size_t count, size_t stride);

} // namespace bybus::translation
