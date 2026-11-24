// Reads the signer certificate SHA-256 from an APK Signing Block (v2/v3).
// This is the native equivalent of PackageManager.GET_SIGNING_CERTIFICATES for
// the one official package; malformed or multi-signer archives fail closed.
#pragma once

#include <string>

namespace bybus::service {

struct ApkSignatureResult {
    bool ok = false;
    std::string error;
    std::string signer_sha256;
};

ApkSignatureResult read_apk_signer(const std::string &apk_path);

} // namespace bybus::service
