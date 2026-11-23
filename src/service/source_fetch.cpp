// service/source_fetch.cpp
// Responsibility: implementation of the three fetch modes. Every external
// program invocation is process_exec with a fixed argv; the URL is validated
// before it becomes an argv element, so no shell metacharacter can appear.
// Source: task contract §7.
#include "service/source_fetch.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "service/process_exec.h"
#include "shared/fs.h"
#include "shared/json.h"
#include "shared/sha256.h"
#include "translation/archive_reader.h"

namespace bybus::service {
namespace {

constexpr const char *kReleaseApi =
    "https://api.github.com/repos/LocalizeLimbusCompany/LocalizeLimbusCompany/releases/latest";
constexpr const char *kCurlSystem = "/system/bin/curl";
constexpr const char *kCurlVendor = "/vendor/bin/curl";
constexpr const char *kWgetSystem = "/system/bin/wget";
constexpr uint64_t kDownloadStdoutBytes = 64 * 1024;

struct Downloader {
    const char *program = nullptr;
    bool curl = true;
};

bool find_downloader(Downloader *out) {
    if (is_executable_file(kCurlSystem)) {
        out->program = kCurlSystem;
        out->curl = true;
        return true;
    }
    if (is_executable_file(kWgetSystem)) {
        out->program = kWgetSystem;
        out->curl = false;
        return true;
    }
    if (is_executable_file(kCurlVendor)) {
        out->program = kCurlVendor;
        out->curl = true;
        return true;
    }
    return false;
}

bool has_https_prefix(const std::string &url) {
    const std::string prefix = "https://";
    return url.size() > prefix.size() && url.compare(0, prefix.size(), prefix) == 0;
}

bool url_char_ok(char c) {
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    switch (c) {
        case ':':
        case '/':
        case '?':
        case '&':
        case '=':
        case '.':
        case '_':
        case '%':
        case '+':
        case '-':
            return true;
        default:
            return false;
    }
}

shared::Code download_to(const std::string &state_root, const std::string &url,
                         const std::string &target, uint64_t max_bytes, int timeout_ms,
                         uint64_t *bytes) {
    Downloader downloader;
    if (!find_downloader(&downloader)) return shared::Code::NetworkUnavailable;
    if (!valid_download_url(url)) return shared::Code::NetworkRejected;
    if (!shared::path_is_inside(state_root, target)) return shared::Code::InvalidArgument;
    (void)::unlink(target.c_str());
    const int seconds = timeout_ms / 1000 > 0 ? timeout_ms / 1000 : 1;
    const std::string timeout_text = std::to_string(seconds);
    std::vector<std::string> args;
    if (downloader.curl) {
        args = {"-fsSL", "--max-time", timeout_text, "--proto", "=https", "-o", target, url};
    } else {
        args = {"-q", "-T", timeout_text, "-O", target, url};
    }
    ExecLimits limits;
    limits.max_output_bytes = kDownloadStdoutBytes;
    limits.timeout_ms = timeout_ms + 5000;
    const ExecResult result = run(downloader.program, args, limits);
    if (!result.spawned) return shared::Code::NetworkUnavailable;
    if (result.timed_out || result.truncated || result.exit_code != 0) {
        (void)::unlink(target.c_str());
        return shared::Code::NetworkRejected;
    }
    struct stat info {};
    if (::stat(target.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return shared::Code::NetworkRejected;
    }
    const uint64_t size = static_cast<uint64_t>(info.st_size);
    if (size > max_bytes) {
        (void)::unlink(target.c_str());
        return shared::Code::ArchiveLimit;
    }
    if (bytes) *bytes = size;
    return shared::Code::Ok;
}

bool asset_version(const std::string &name, std::string *version) {
    const std::string prefix = "LimbusLocalize_";
    const std::string suffix = ".7z";
    if (name.size() <= prefix.size() + suffix.size()) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const std::string digits =
        name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    for (const char c : digits) {
        if (c < '0' || c > '9') return false;
    }
    if (version) *version = digits;
    return true;
}

bool version_newer(const std::string &left, const std::string &right) {
    if (left.size() != right.size()) return left.size() > right.size();
    return left > right;
}

std::string digest_from_asset(const shared::JsonPtr &asset) {
    static const char *const kKeys[] = {"digest", "sha256"};
    for (const char *key : kKeys) {
        const shared::JsonPtr value = asset->opt(key);
        if (!value || !value->is_string()) continue;
        std::string text = value->as_string();
        const std::string prefix = "sha256:";
        if (text.size() > prefix.size() && text.compare(0, prefix.size(), prefix) == 0) {
            text = text.substr(prefix.size());
        }
        if (shared::is_sha256_hex(text)) return text;
    }
    return std::string();
}

} // namespace

bool valid_download_url(const std::string &url) {
    if (!has_https_prefix(url)) return false;
    if (url.size() > kMaxUrlBytes) return false;
    for (const char c : url) {
        if (!url_char_ok(c)) return false;
    }
    const size_t host_start = std::string("https://").size();
    const size_t host_end = url.find('/', host_start);
    const size_t host_length =
        host_end == std::string::npos ? url.size() - host_start : host_end - host_start;
    return host_length >= 3;
}

bool release_asset_name(const std::string &name, std::string *version) {
    return asset_version(name, version);
}

bool valid_release_asset_url(const std::string &url) {
    static const std::string prefix =
        "https://github.com/LocalizeLimbusCompany/LocalizeLimbusCompany/releases/download/";
    if (!valid_download_url(url) || url.compare(0, prefix.size(), prefix) != 0) return false;
    const size_t slash = url.find('/', prefix.size());
    if (slash == std::string::npos || slash == prefix.size() ||
        url.find('/', slash + 1) != std::string::npos) return false;
    const std::string tag = url.substr(prefix.size(), slash - prefix.size());
    if (tag == "." || tag == ".." || tag.find("..") != std::string::npos) return false;
    for (const char c : tag) {
        const bool accepted = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                              c == '%' || c == '+' || c == '-';
        if (!accepted) return false;
    }
    return asset_version(url.substr(slash + 1), nullptr);
}

shared::Code fetch_release(const ServicePaths &paths, const std::string &work_dir,
                           FetchOutcome *out) {
    if (out == nullptr) return shared::Code::InvalidArgument;
    if (!shared::path_is_inside(paths.state_root, work_dir)) {
        return shared::Code::InvalidArgument;
    }
    const std::string metadata_path = work_dir + "/release.json";
    uint64_t metadata_bytes = 0;
    shared::Code code = download_to(paths.state_root, kReleaseApi, metadata_path,
                                    kMaxReleaseJsonBytes, kReleaseJsonTimeoutMillis,
                                    &metadata_bytes);
    if (code != shared::Code::Ok) return code;
    std::string text;
    code = shared::read_file(metadata_path, kMaxReleaseJsonBytes, &text);
    if (code != shared::Code::Ok) return code;
    const shared::ParseOutcome parsed = shared::parse(text);
    if (!parsed.ok || !parsed.value || !parsed.value->is_object()) {
        return shared::Code::NetworkRejected;
    }
    const shared::JsonPtr assets = parsed.value->opt("assets");
    if (!assets || !assets->is_array()) return shared::Code::NetworkRejected;
    std::string best_version;
    std::string best_name;
    std::string best_url;
    std::string best_digest;
    for (size_t index = 0; index < assets->length(); ++index) {
        const shared::JsonPtr asset = assets->at(index);
        if (!asset || !asset->is_object()) continue;
        const shared::JsonPtr name = asset->opt("name");
        const shared::JsonPtr url = asset->opt("browser_download_url");
        if (!name || !name->is_string() || !url || !url->is_string()) continue;
        std::string version;
        if (!asset_version(name->as_string(), &version)) continue;
        if (!valid_download_url(url->as_string())) continue;
        if (!best_version.empty() && !version_newer(version, best_version)) continue;
        best_version = version;
        best_name = name->as_string();
        best_url = url->as_string();
        best_digest = digest_from_asset(asset);
    }
    if (best_url.empty()) return shared::Code::NetworkRejected;
    const std::string target = work_dir + "/release.7z";
    uint64_t bytes = 0;
    code = download_to(paths.state_root, best_url, target, kMaxArchiveBytes,
                       kDownloadTimeoutMillis, &bytes);
    if (code != shared::Code::Ok) return code;
    out->code = shared::Code::Ok;
    out->archive_path = target;
    out->display_name = best_name;
    out->expected_sha256 = best_digest;
    out->bytes = bytes;
    return shared::Code::Ok;
}

shared::Code fetch_inbox(const ServicePaths &paths, FetchOutcome *out) {
    if (out == nullptr) return shared::Code::InvalidArgument;
    std::vector<std::string> names;
    if (shared::list_dir(paths.inbox(), kMaxInboxFiles, &names) != shared::Code::Ok) {
        return shared::Code::ArchiveInvalid;
    }
    std::string best_path;
    std::string best_name;
    int64_t best_time = -1;
    for (const std::string &name : names) {
        if (name.empty() || name.front() == '.') continue;
        const std::string path = paths.inbox() + "/" + name;
        struct stat info {};
        if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
        if (translation::detect_format(path) == translation::ArchiveFormat::Unknown) continue;
        const int64_t stamp = static_cast<int64_t>(info.st_mtime);
        if (stamp < best_time) continue;
        best_time = stamp;
        best_path = path;
        best_name = name;
    }
    if (best_path.empty()) return shared::Code::ArchiveInvalid;
    if (!inside_state_root(paths, best_path)) return shared::Code::ArchivePathUnsafe;
    out->code = shared::Code::Ok;
    out->archive_path = best_path;
    out->display_name = best_name;
    out->bytes = shared::file_size(best_path);
    return shared::Code::Ok;
}

shared::Code fetch_url(const ServicePaths &paths, const std::string &url,
                       const std::string &work_dir, FetchOutcome *out) {
    if (out == nullptr) return shared::Code::InvalidArgument;
    if (!valid_download_url(url)) return shared::Code::InvalidArgument;
    if (!shared::path_is_inside(paths.state_root, work_dir)) {
        return shared::Code::InvalidArgument;
    }
    const std::string target = work_dir + "/url.pack";
    uint64_t bytes = 0;
    const shared::Code code = download_to(paths.state_root, url, target, kMaxArchiveBytes,
                                          kDownloadTimeoutMillis, &bytes);
    if (code != shared::Code::Ok) return code;
    out->code = shared::Code::Ok;
    out->archive_path = target;
    const size_t slash = url.rfind('/');
    out->display_name = slash == std::string::npos ? std::string("archive") : url.substr(slash + 1);
    out->bytes = bytes;
    return shared::Code::Ok;
}

} // namespace bybus::service
