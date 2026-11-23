// Builds one immutable generation directory from verified inputs. Pure data +
// bounded file writes inside the supplied work directory; publishing (rename +
// active pointer) belongs to the service layer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shared/status.h"
#include "translation/archive_import.h"
#include "translation/snapshot.h"

namespace bybus::translation {

struct GameTable {
    std::string resource;  // canonical Localize/<locale>/... name
    std::string content;   // UTF-8 text exactly as the loader returns it
};

struct BuildInputs {
    std::string work_dir;                 // existing empty directory
    std::string source_id;
    std::string archive_sha256;
    int64_t game_version_code = 0;
    std::string game_install_identity;
    std::string package_name;             // verified installed package name
    std::string game_apk_path;            // installed base.apk used for the profile
    std::vector<std::string> game_apk_paths; // all installed APK splits, in pm order
    std::string font_source_path;         // empty when no font is published
    std::vector<GameTable> originals;
    std::vector<ImportedTable> pack;
};

struct BuildOutcome {
    shared::Code code = shared::Code::Ok;
    std::string detail;
    std::string generation;
    uint32_t tables = 0;          // entries written
    uint32_t skipped = 0;         // originals with no display change or invalid input
    uint32_t matched_fields = 0;
    uint32_t source_fields = 0;
    // False when the APK could not be profiled: the runtime must not patch.
    bool profile_present = false;
};

// Pairs every original with the pack, writes tables/, font and manifest.json.
// A table whose original cannot be parsed is skipped and counted, never
// substituted by a global string replacement.
BuildOutcome build_generation(const BuildInputs &inputs);

} // namespace bybus::translation
