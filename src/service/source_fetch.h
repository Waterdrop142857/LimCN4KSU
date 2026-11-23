// service/source_fetch.h
// Responsibility: turn one of the three update modes into a local archive
// file. `release` polls the fixed GitHub release and picks
// LimbusLocalize_<digits>.7z; `inbox` takes the newest .7z/.zip the user
// dropped into inbox/; `url` accepts one validated https URL.
// Downloads are delegated to /system/bin/curl (then /system/bin/wget, then
// /vendor/bin/curl) with fixed argv and no shell. Source: task contract §7,
// 10-port-plan.md §3.2.
#pragma once

#include <cstdint>
#include <string>

#include "service/paths.h"
#include "shared/status.h"

namespace bybus::service {

struct FetchOutcome {
    shared::Code code = shared::Code::Ok;
    std::string archive_path;     // local file the pipeline validates next
    std::string display_name;     // asset name or inbox file name
    std::string expected_sha256;  // published digest, empty when unknown
    uint64_t bytes = 0;
};

// https only, <= 2048 bytes, charset [A-Za-z0-9:/?&=._%+-].
bool valid_download_url(const std::string &url);

// All modes write into `work_dir` (an existing scratch directory inside the
// state root) except `inbox`, which returns the user's file in place.
shared::Code fetch_release(const ServicePaths &paths, const std::string &work_dir,
                           FetchOutcome *out);
shared::Code fetch_inbox(const ServicePaths &paths, FetchOutcome *out);
shared::Code fetch_url(const ServicePaths &paths, const std::string &url,
                       const std::string &work_dir, FetchOutcome *out);

// True when the file name matches LimbusLocalize_<digits>.7z; `version` gets
// the decimal version string when it does.
bool release_asset_name(const std::string &name, std::string *version);
// Only a direct public asset URL from the fixed official repository is accepted.
bool valid_release_asset_url(const std::string &url);

} // namespace bybus::service
