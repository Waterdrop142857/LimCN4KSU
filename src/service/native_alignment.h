// Bounded APK native-library qualification ported from
// LimbusNativeProfile.requireLibraryAlignment / LimbusNativeStructure.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bybus::service {

struct NativeAlignmentResult {
    bool ok = false;
    std::string error;
    uint32_t libraries = 0;
};

// Reads every lib/arm64-v8a/*.so header from all installed APK splits and
// verifies page alignment, required libraries and duplicate names.
NativeAlignmentResult validate_native_alignment(const std::vector<std::string> &apk_paths,
                                                uint64_t page_size);

} // namespace bybus::service
