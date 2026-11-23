// service/paths.h
// Responsibility: the single place that knows the private state layout
// (/data/adb/bybus, 10-port-plan.md §1 and 20-protocol.md §1), the numeric
// limits the service layers share, and the small clock / random / identifier
// helpers used by the daemon. Source of values: 10-port-plan.md §1, §4 and
// 20-protocol.md §1–§5, 12-kernelsu-appsealing.md §4.
// This header performs no I/O and knows nothing about the protocol shape.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "shared/status.h"

namespace bybus::service {

// --- on-device locations -------------------------------------------------
inline constexpr const char *kDefaultStateRoot = "/data/adb/bybus";
inline constexpr const char *kDefaultModuleRoot = "/data/adb/modules/bybus";
inline constexpr const char *kSocketName = "bybus.sock";
inline constexpr const char *kActiveName = "active";
inline constexpr const char *kStateName = "state.json";
inline constexpr const char *kDefaultSourceId = "zero";
inline constexpr const char *kDefaultSourceName = "\u90fd\u5e02\u96f6\u534f\u4f1a";
inline constexpr const char *kLocalSourcePrefix = "local-";
inline constexpr const char *kGamePackage = "com.ProjectMoon.LimbusCompany";
inline constexpr const char *kFontRelative = "assets/ChineseFont.ttf";
inline constexpr const char *kFontDigestRelative = "assets/font.sha256";

// --- limits (20-protocol.md §1, task contract §5–§9) --------------------
inline constexpr uint64_t kMaxArchiveBytes = UINT64_MAX;
inline constexpr uint64_t kImportChunkBytes = 262144;
inline constexpr uint64_t kMaxRequestBytes = 256ull * 1024;
inline constexpr uint64_t kMaxResponseBytes = 4ull * 1024 * 1024;
inline constexpr uint64_t kStorageLimitBytes = 0; // no content quota
inline constexpr uint64_t kMaxGameTableBytes = UINT64_MAX;
inline constexpr uint64_t kMaxGameTableTotal = UINT64_MAX;
inline constexpr uint32_t kMaxGameTables = UINT32_MAX;
inline constexpr uint32_t kKeepGenerations = 2;
inline constexpr size_t kMaxConnections = 8;
inline constexpr int kConnectionReadMillis = 15000;
inline constexpr int64_t kImportSessionTtlMillis = 2 * 60 * 60 * 1000;
inline constexpr int64_t kGameProbeTtlMillis = 10 * 1000;
inline constexpr uint64_t kMaxManifestBytes = UINT64_MAX;
inline constexpr uint64_t kMaxFontDigestBytes = 4096;
inline constexpr uint64_t kMaxReceiptBytes = 64ull * 1024;
inline constexpr uint64_t kMaxStateBytes = 1024ull * 1024;
inline constexpr uint64_t kMaxJobFileBytes = 64ull * 1024;
inline constexpr uint64_t kMaxLogBytes = 1024ull * 1024;
inline constexpr uint64_t kMaxReleaseJsonBytes = 1024ull * 1024;
inline constexpr size_t kMaxDiagnosticsEntries = 200;
inline constexpr uint32_t kMaxSourceFiles = UINT32_MAX;
inline constexpr uint32_t kMaxJobFiles = UINT32_MAX;
inline constexpr uint32_t kMaxGenerationsListed = UINT32_MAX;
inline constexpr uint32_t kMaxInboxFiles = UINT32_MAX;
inline constexpr int kDownloadTimeoutMillis = 600000;
inline constexpr int kReleaseJsonTimeoutMillis = 60000;
inline constexpr int kInstallProbeMillis = 3000;
inline constexpr int kVersionProbeMillis = 2000;
inline constexpr uint64_t kInstallProbeOutputBytes = 1024ull * 1024;
inline constexpr uint32_t kMaxPackagePaths = 8;
inline constexpr size_t kMaxPackageLineBytes = 1024;
inline constexpr size_t kMaxUrlBytes = 2048;
inline constexpr size_t kMaxImportNameBytes = 256;

// --- layout ------------------------------------------------------------

// Every directory the daemon owns inside the private state root. Created with
// mode 0700 before any job starts.
struct ServicePaths {
    std::string state_root = kDefaultStateRoot;
    std::string module_root = kDefaultModuleRoot;

    std::string socket() const { return state_root + "/" + kSocketName; }
    std::string sources() const { return state_root + "/sources"; }
    std::string archives() const { return state_root + "/archives"; }
    std::string imports() const { return state_root + "/imports"; }
    std::string generations() const { return state_root + "/generations"; }
    std::string jobs() const { return state_root + "/jobs"; }
    std::string logs() const { return state_root + "/logs"; }
    std::string runtime() const { return state_root + "/runtime"; }
    std::string inbox() const { return state_root + "/inbox"; }
    std::string run() const { return state_root + "/run"; }

    std::string active() const { return state_root + "/" + kActiveName; }
    std::string active_temp() const { return state_root + "/active.tmp"; }
    std::string state_file() const { return state_root + "/" + kStateName; }
    std::string pid_file() const { return run() + "/bybusd.pid"; }
    std::string lock_file() const { return run() + "/bybusd.lock"; }
    std::string log_file() const { return logs() + "/bybusd.log"; }
    std::string diagnostics_file() const { return logs() + "/diagnostics.log"; }
    std::string receipt_file() const { return runtime() + "/receipt.json"; }
    std::string font_source() const { return module_root + "/" + kFontRelative; }
    std::string font_digest() const { return module_root + "/" + kFontDigestRelative; }
    std::string module_prop() const { return module_root + "/module.prop"; }
    std::string source_file(const std::string &id) const { return sources() + "/" + id + ".json"; }
    std::string job_file(const std::string &id) const { return jobs() + "/" + id + ".json"; }
    std::string generation_dir(const std::string &id) const { return generations() + "/" + id; }
    std::string archive_file(const std::string &sha256) const { return archives() + "/" + sha256 + ".pack"; }
    std::string import_dir(const std::string &session) const { return imports() + "/" + session; }
};

// --- helpers -----------------------------------------------------------

int64_t now_millis();
int64_t monotonic_millis();

// Lowercase hex from /dev/urandom; falls back to a mixed clock/pid stream.
std::string random_hex(size_t characters);

// "<prefix><hex chars>" with lowercase hex only.
bool valid_hex_token(const std::string &value, const char *prefix, size_t hex_chars);
bool valid_job_id(const std::string &value);
bool valid_session_id(const std::string &value);
// [a-z0-9-], 1..64 characters, not starting with '-'.
bool valid_source_id(const std::string &value);
// Absolute path without ".." segments and without control characters.
bool valid_absolute_path(const std::string &value);

// Creates the whole private layout (0700). Refuses a relative or unsafe root.
shared::Code ensure_layout(const ServicePaths &paths);

// True when `candidate` is a strict descendant of the state root.
bool inside_state_root(const ServicePaths &paths, const std::string &candidate);

// write_file into "<path>.tmp" then rename; fsyncs both sides.
shared::Code write_atomic(const std::string &path, const std::string &content);

} // namespace bybus::service
