// AppSealing compatibility injection for the official game process.
//
// Ported from BYBUS `Foundation/LimbusCodeIntegrityCompat.cpp`,
// `Foundation/LimbusReportBridge.h` and the AppSealing site patches in
// `Foundation/IORelocator64.cpp` (commit bd32608), adapted to the KernelSU
// runtime: no VirtualApp container, no syscall hooks and no MapsRedirector.
//
// Every offset and instruction word comes from the snapshot manifest, which
// bybusd produced locally from the installed APK. The runtime only applies the
// plan after re-checking the live library.
#pragma once

#include <jni.h>

#include <cstdint>
#include <string>

namespace bybus::runtime {

enum class CompatState { None, Installed, Rejected };

CompatState compat_state();
const char *compat_state_text(CompatState state);
void set_java_vm(JavaVM *vm);
bool install_appsealing_compat(std::string *error);
void maintain_appsealing_compat();
uint64_t compat_maintain_count();
uintptr_t appsealing_protection_base();

} // namespace bybus::runtime
