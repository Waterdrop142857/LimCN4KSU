// service/game_scan.cpp
// Responsibility: implementation of the install probe and the read-only text
// enumeration. External programs are only ever `pm` and `dumpsys` at their
// system paths, through process_exec (fixed argv, timeout, output cap).
#include "service/game_scan.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "service/process_exec.h"
#include "shared/fs.h"
#include "shared/sha256.h"

namespace bybus::service {
namespace {

constexpr const char *kPmPath = "/system/bin/pm";
constexpr const char *kDumpsysPath = "/system/bin/dumpsys";
constexpr const char *kLocales[3] = {"jp", "en", "kr"};

// Ordered exactly as the contract lists them; the first root containing text wins.
const char *source_root(size_t index) {
    switch (index) {
        case 0:
            return "/storage/emulated/0/Android/data/com.ProjectMoon.LimbusCompany/files";
        case 1:
            return "/sdcard/Android/data/com.ProjectMoon.LimbusCompany/files";
        case 2:
            return "/data/data/com.ProjectMoon.LimbusCompany/files";
        default:
            return "/data/user/0/com.ProjectMoon.LimbusCompany/files";
    }
}

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '\t')) {
        text.pop_back();
    }
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    return text.substr(begin);
}

char lower_ascii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool ends_with_json(const std::string &name) {
    static const char *const kSuffix = ".json";
    const size_t length = 5;
    if (name.size() <= length) return false;
    const size_t offset = name.size() - length;
    for (size_t index = 0; index < length; ++index) {
        if (lower_ascii(name[offset + index]) != kSuffix[index]) return false;
    }
    return true;
}

// `pm path <pkg>` prints one "package:<apk path>" line per split.
std::vector<std::string> parse_package_paths(const std::string &output) {
    std::vector<std::string> paths;
    size_t start = 0;
    while (start < output.size() && paths.size() < kMaxPackagePaths) {
        const size_t newline = output.find('\n', start);
        const size_t end = newline == std::string::npos ? output.size() : newline;
        const std::string line = trim(output.substr(start, end - start));
        if (line.size() > 8 && line.compare(0, 8, "package:") == 0 && line[8] == '/') {
            const std::string path = line.substr(8);
            if (path.find("..") == std::string::npos && path.size() <= 4096) {
                paths.push_back(path);
            }
        }
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    return paths;
}

void parse_version_fields(const std::string &output, GameInstall *install) {
    const std::string code_key = "versionCode=";
    const size_t code_at = output.find(code_key);
    if (code_at != std::string::npos) {
        size_t cursor = code_at + code_key.size();
        int64_t value = 0;
        bool any = false;
        while (cursor < output.size() && output[cursor] >= '0' && output[cursor] <= '9') {
            if (value < 100000000000LL) value = value * 10 + (output[cursor] - '0');
            any = true;
            ++cursor;
        }
        if (any) install->version_code = value;
    }
    const std::string name_key = "versionName=";
    const size_t name_at = output.find(name_key);
    if (name_at != std::string::npos) {
        size_t cursor = name_at + name_key.size();
        std::string value;
        while (cursor < output.size() && value.size() < 64) {
            const char c = output[cursor];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') break;
            value.push_back(c);
            ++cursor;
        }
        install->version_name = value;
    }
}

std::string identity_of(const std::vector<std::string> &paths) {
    std::string joined;
    for (const std::string &path : paths) {
        struct stat info {};
        if (::stat(path.c_str(), &info) != 0) continue;
        joined += "package|";
        joined += path;
        joined.push_back('|');
        joined += std::to_string(static_cast<long long>(info.st_size));
        joined.push_back('|');
        joined += std::to_string(static_cast<long long>(info.st_mtime));
        joined.push_back('|');
    }
    if (joined.empty()) return std::string();
    return shared::Sha256::hex(joined);
}

// Finds <root>/Localize within `depth` directory levels below `root`.
bool find_localize(const std::string &root, int depth, std::string *found) {
    const std::string direct = root + "/Localize";
    if (shared::is_directory(direct)) {
        *found = direct;
        return true;
    }
    if (depth <= 0) return false;
    std::vector<std::string> names;
    if (shared::list_dir(root, 64, &names) != shared::Code::Ok) return false;
    for (const std::string &name : names) {
        if (name.empty() || name.front() == '.') continue;
        const std::string child = root + "/" + name;
        if (!shared::is_directory(child)) continue;
        if (find_localize(child, depth - 1, found)) return true;
    }
    return false;
}

// Adds one candidate file. A discovered JSON file that cannot be inspected or
// read must fail the scan, rather than silently publishing a partial snapshot.
bool add_text_file(const std::string &path, const std::string &relative,
                   const std::string &locale, bool load_contents, uint64_t *total_bytes,
                   GameTextScan *out) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        out->code = shared::Code::IoError;
        return false;
    }
    if (!S_ISREG(info.st_mode)) return true;
    const uint64_t bytes = static_cast<uint64_t>(info.st_size);
    if (*total_bytes > UINT64_MAX - bytes) {
        out->code = shared::Code::ArchiveLimit;
        return false;
    }
    GameTextFile file;
    file.path = path;
    file.resource = "Localize/" + relative;
    file.bytes = bytes;
    bool known = false;
    for (const std::string &item : out->locales) {
        if (item == locale) known = true;
    }
    if (!known) out->locales.push_back(locale);
    out->files.push_back(file);
    *total_bytes += bytes;
    if (!load_contents) return true;
    std::string content;
    const shared::Code read = shared::read_file(path, kMaxGameTableBytes, &content);
    if (read != shared::Code::Ok) {
        out->code = read;
        return false;
    }
    translation::GameTable table;
    table.resource = file.resource;
    table.content = std::move(content);
    out->tables.push_back(std::move(table));
    return true;
}

bool collect_locale_files(const std::string &locale_dir, const std::string &locale,
                          bool load_contents, uint64_t *total_bytes, GameTextScan *out) {
    std::vector<std::string> names;
    const shared::Code listed = shared::list_dir(locale_dir, kMaxGameTables, &names);
    if (listed != shared::Code::Ok) {
        out->code = listed;
        return false;
    }
    for (const std::string &name : names) {
        if (out->files.size() >= kMaxGameTables) return false;
        if (name.empty() || name.front() == '.') continue;
        const std::string path = locale_dir + "/" + name;
        if (shared::is_directory(path)) {
            std::vector<std::string> nested;
            const shared::Code nested_listed = shared::list_dir(path, kMaxGameTables, &nested);
            if (nested_listed != shared::Code::Ok) {
                out->code = nested_listed;
                return false;
            }
            for (const std::string &child : nested) {
                if (out->files.size() >= kMaxGameTables) return false;
                if (child.empty() || child.front() == '.') continue;
                if (!ends_with_json(child)) continue;
                const std::string child_path = path + "/" + child;
                if (shared::is_directory(child_path)) continue;
                if (!add_text_file(child_path, locale + "/" + name + "/" + child, locale,
                                   load_contents, total_bytes, out)) {
                    return false;
                }
            }
            continue;
        }
        if (!ends_with_json(name)) continue;
        if (!add_text_file(path, locale + "/" + name, locale, load_contents, total_bytes, out)) {
            return false;
        }
    }
    return true;
}

} // namespace

shared::Code probe_install(GameInstall *out, Logger *log) {
    if (out == nullptr) return shared::Code::InvalidArgument;
    GameInstall install;
    if (!is_executable_file(kPmPath)) {
        if (log) log->error("probe pm_missing");
        *out = install;
        return shared::Code::GameMissing;
    }
    ExecLimits limits;
    limits.max_output_bytes = kInstallProbeOutputBytes;
    limits.timeout_ms = kInstallProbeMillis;
    limits.max_lines = kMaxPackagePaths;
    limits.max_line_bytes = kMaxPackageLineBytes;
    const std::vector<std::string> path_args = {"path", kGamePackage};
    const ExecResult listed = run(kPmPath, path_args, limits);
    if (!listed.spawned || listed.timed_out || listed.exit_code != 0) {
        if (log) log->error("probe pm_path_failed");
        *out = install;
        return shared::Code::GameMissing;
    }
    install.apk_paths = parse_package_paths(listed.output);
    install.install_identity = identity_of(install.apk_paths);
    if (install.apk_paths.empty() || install.install_identity.empty()) {
        *out = install;
        return shared::Code::GameMissing;
    }
    install.installed = true;
    if (!is_executable_file(kDumpsysPath)) {
        *out = install;
        return shared::Code::Ok;
    }
    ExecLimits version_limits;
    version_limits.max_output_bytes = kInstallProbeOutputBytes;
    version_limits.timeout_ms = kVersionProbeMillis;
    const std::vector<std::string> dump_args = {"package", kGamePackage};
    const ExecResult dumped = run(kDumpsysPath, dump_args, version_limits);
    if (dumped.spawned && !dumped.timed_out && dumped.exit_code == 0) {
        parse_version_fields(dumped.output, &install);
    }
    *out = install;
    return shared::Code::Ok;
}

shared::Code scan_game_texts(const GameInstall &install, bool load_contents, GameTextScan *out) {
    if (out == nullptr) return shared::Code::InvalidArgument;
    *out = GameTextScan();
    out->loaded_contents = load_contents;
    if (!install.installed) {
        out->code = shared::Code::GameMissing;
        return out->code;
    }
    std::string localize;
    for (size_t index = 0; index < 4; ++index) {
        const std::string candidate = source_root(index);
        if (shared::is_directory(candidate) && find_localize(candidate, 2, &localize)) break;
    }
    if (localize.empty()) {
        out->code = shared::Code::GameMissingText;
        return out->code;
    }
    out->root = localize;
    uint64_t total_bytes = 0;
    for (size_t index = 0; index < 3; ++index) {
        const std::string locale = kLocales[index];
        const std::string dir = localize + "/" + locale;
        if (!shared::is_directory(dir)) continue;
        if (!collect_locale_files(dir, locale, load_contents, &total_bytes, out)) break;
    }
    if (out->code != shared::Code::Ok) return out->code;
    if (out->files.empty()) {
        out->code = shared::Code::GameMissingText;
        return out->code;
    }
    out->code = shared::Code::Ok;
    return shared::Code::Ok;
}

GameProbe::GameProbe() = default;

void GameProbe::snapshot(bool force, GameInstall *install, GameTextScan *texts) {
    {
        std::lock_guard<std::mutex> guard(state_mu_);
        if (!force && valid_ && now_millis() - at_millis_ < kGameProbeTtlMillis) {
            if (install) *install = install_;
            if (texts) *texts = texts_;
            return;
        }
    }
    std::lock_guard<std::mutex> probe_guard(probe_mu_);
    {
        std::lock_guard<std::mutex> guard(state_mu_);
        if (!force && valid_ && now_millis() - at_millis_ < kGameProbeTtlMillis) {
            if (install) *install = install_;
            if (texts) *texts = texts_;
            return;
        }
    }
    GameInstall probed;
    GameTextScan scanned;
    const shared::Code code = probe_install(&probed, nullptr);
    if (probed.installed) {
        (void)scan_game_texts(probed, false, &scanned);
    } else {
        scanned.code = code;
    }
    {
        std::lock_guard<std::mutex> guard(state_mu_);
        install_ = probed;
        texts_ = scanned;
        at_millis_ = now_millis();
        valid_ = true;
        if (install) *install = install_;
        if (texts) *texts = texts_;
    }
}

void GameProbe::invalidate() {
    std::lock_guard<std::mutex> guard(state_mu_);
    valid_ = false;
    at_millis_ = 0;
}

} // namespace bybus::service
