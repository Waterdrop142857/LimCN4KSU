// Installs the two read-after-load text hooks and owns their replacements.
// Only localization read paths are touched; resource descriptors, downloads and
// save data are never redirected.
#pragma once

namespace bybus::runtime {

// Resolves classes/methods, records the patch in the ledger, installs the hooks
// and verifies the integrity profile. Rolls back and returns false on any
// mismatch, leaving the process unpatched.
bool install_text_hooks();

// Hash the snapshot font on the monitor thread before any text callback can
// run. The main-thread font gate only handles Unity/TMP API calls.
bool verify_font_snapshot();

// Font gate: the Chinese font handle is only accepted when Unity itself can
// build a dynamic font from it.
bool prepare_font();

} // namespace bybus::runtime
