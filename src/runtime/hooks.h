// Installs the read-after-load text hooks and owns their replacements.
// Only localization read paths are touched; resource descriptors, downloads and
// save data are never redirected.
#pragma once

namespace bybus::runtime {

// Resolves classes/methods, records the patch in the ledger, installs the hooks
// and records an integrity audit. Returns whether the entry hooks are ready:
// LoadLocalizeFileSync, TextAsset.get_text and TMP_Text.set_text must all own a
// trampoline. An optional hook that fails is only logged and never rolls the
// rest back; text_entry_installed() reports the same state.
bool install_text_hooks();

// Hash the snapshot font on the monitor thread before any text callback can
// run. The main-thread font gate only handles Unity/TMP API calls.
bool verify_font_snapshot();

// Font gate: the Chinese font handle is only accepted when Unity itself can
// build a dynamic font from it.
bool prepare_font();

} // namespace bybus::runtime
