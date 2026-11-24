// Minimal binary AndroidManifest.xml reader for the installed APK. Extracts
// the package name and <application android:name> without aapt.
#pragma once

#include <string>

namespace bybus::service {

struct AndroidManifestInfo {
    bool ok = false;
    std::string error;
    std::string package_name;
    std::string application_class;
};

AndroidManifestInfo read_android_manifest(const std::string &apk_path);

} // namespace bybus::service
