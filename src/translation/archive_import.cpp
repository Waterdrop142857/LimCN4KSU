#include "translation/archive_import.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "shared/sha256.h"
#include "shared/utf.h"
#include "translation/json_pairer.h"

namespace bybus::translation {
namespace {

constexpr size_t kMaxPathLength = 512;

bool starts_with(const std::string &text, const std::string &prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with_ignore_case(const std::string &text, const std::string &suffix) {
    if (text.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin(), [](char left, char right) {
        const char l = left >= 'A' && left <= 'Z' ? static_cast<char>(left - 'A' + 'a') : left;
        const char r = right >= 'A' && right <= 'Z' ? static_cast<char>(right - 'A' + 'a') : right;
        return l == r;
    });
}

std::string basename_of(const std::string &path) {
    const size_t at = path.rfind('/');
    return at == std::string::npos ? path : path.substr(at + 1);
}

std::string dirname_of(const std::string &path) {
    const size_t at = path.rfind('/');
    return at == std::string::npos ? std::string() : path.substr(0, at);
}

bool is_known_resource_folder(const std::string &folder) {
    static const char *const kFolders[] = {"StoryData",  "BattleHint",     "RPGSystem",
                                           "EGOVoiceDig", "PersonalityVoiceDlg", "BattleAnnouncerDlg"};
    for (const char *candidate : kFolders) {
        if (folder == candidate) return true;
    }
    return false;
}

class ImportVisitor final : public ArchiveVisitor {
public:
    explicit ImportVisitor(ImportResult *result) : result_(result) {}

    bool on_entry(const ArchiveEntry &entry, bool *skip) override {
        ++entry_count_;
        if (entry.directory) {
            *skip = true;
            return true;
        }
        const std::string normalized = safe_path(entry.raw_name);
        if (normalized.empty()) {
            detail_ = "archive_path_unsafe";
            return false;
        }
        resource_ = map_resource(normalized);
        raw_entry_ = normalized;
        if (resource_.empty()) {
            *skip = true;
            return true;
        }
        *skip = false;
        buffer_.clear();
        return true;
    }

    bool on_data(const char *data, size_t size) override {
        buffer_.append(data, size);
        return true;
    }

    bool on_entry_end() override {
        if (resource_.empty()) return true;
        if (!shared::valid_utf8(buffer_)) {
            detail_ = "archive_text_encoding";
            return false;
        }
        std::vector<std::string> texts;
        std::string error;
        if (!JsonPairer::released_texts(buffer_, &texts, &error)) {
            detail_ = "archive_text_invalid";
            return false;
        }
        std::string lowered = resource_;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        if (!seen_.insert(lowered).second) {
            detail_ = "archive_duplicate";
            return false;
        }
        ImportedTable table;
        table.raw_entry = raw_entry_;
        table.resource_name = resource_;
        table.relative_path = "files/en/" + resource_;
        table.content = buffer_;
        table.bytes = buffer_.size();
        table.sha256 = shared::Sha256::hex(buffer_);
        result_->tables.push_back(std::move(table));
        resource_.clear();
        buffer_.clear();
        return true;
    }

    const std::string &detail() const { return detail_; }
    uint32_t entry_count() const { return entry_count_; }
    size_t table_count() const { return result_->tables.size(); }

private:
    ImportResult *result_;
    std::string raw_entry_;
    std::string resource_;
    std::string buffer_;
    std::string detail_;
    std::unordered_set<std::string> seen_;
    uint32_t entry_count_ = 0;
};

} // namespace

std::string safe_path(const std::string &raw) {
    std::string path;
    path.reserve(raw.size());
    for (const char c : raw) path.push_back(c == '\\' ? '/' : c);
    while (!path.empty() && path.back() == '/') path.pop_back();
    if (path.empty() || path.size() > kMaxPathLength) return std::string();
    if (path.front() == '/' || path.find(':') != std::string::npos) return std::string();
    for (const char c : path) {
        if (static_cast<unsigned char>(c) < 0x20) return std::string();
    }
    size_t start = 0;
    while (start <= path.size()) {
        const size_t separator = path.find('/', start);
        const size_t end = separator == std::string::npos ? path.size() : separator;
        const std::string segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") return std::string();
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return path;
}

std::string map_resource(const std::string &path) {
    if (!ends_with_ignore_case(path, ".json")) return std::string();
    static const char *const kPrefixes[] = {
        "LimbusCompany_Data/Lang/LLC_zh-CN/", "LLC_zh-CN/", "Localize/jp/", "Localize/en/",
        "Localize/kr/",                      "jp/",        "en/",          "kr/",
        "files/en/",
    };
    const char *matched = nullptr;
    for (const char *prefix : kPrefixes) {
        if (starts_with(path, prefix)) {
            matched = prefix;
            break;
        }
    }
    const std::string relative =
        matched == nullptr ? path : path.substr(std::string(matched).size());
    if (starts_with(relative, "Info/")) return std::string();
    const std::string name = basename_of(relative);
    if (name == "manifest.json" || name == "bybus-pack.json" || name == "version.json") {
        return std::string();
    }
    if (matched == nullptr && relative.find('/') != std::string::npos &&
        !is_known_resource_folder(relative.substr(0, relative.find('/')))) {
        return std::string();
    }
    const std::string folder = dirname_of(relative);
    std::string file = name;
    static const char *const kLanguagePrefixes[] = {"EN_", "JP_", "KR_"};
    for (const char *prefix : kLanguagePrefixes) {
        if (starts_with(file, prefix)) {
            file = file.substr(3);
            break;
        }
    }
    return (folder.empty() ? std::string() : folder + "/") + "EN_" + file;
}

shared::Code import_archive(const std::string &archive_path, ImportResult *out) {
    ImportResult result;
    ImportVisitor visitor(&result);
    const ArchiveFormat format = detect_format(archive_path);
    if (format == ArchiveFormat::Unknown) {
        out->code = shared::Code::ArchiveInvalid;
        out->detail = "archive_format_unknown";
        return out->code;
    }
    const ArchiveResult read = read_archive(archive_path, visitor);
    // The visitor is the only party that knows why the import stopped, so its
    // detail wins over the generic container status.
    if (!visitor.detail().empty()) {
        const std::string &detail = visitor.detail();
        shared::Code code = shared::Code::ArchiveInvalid;
        if (detail == "archive_duplicate") {
            code = shared::Code::ArchiveDuplicate;
        } else if (detail == "archive_path_unsafe") {
            code = shared::Code::ArchivePathUnsafe;
        }
        out->code = code;
        out->detail = detail;
        return out->code;
    }
    if (read.code != shared::Code::Ok) {
        out->code = read.code;
        out->detail = read.detail;
        return out->code;
    }
    if (result.tables.empty()) {
        out->code = shared::Code::ArchiveInvalid;
        out->detail = "archive_no_text";
        return out->code;
    }
    result.entry_count = visitor.entry_count();
    result.archive_bytes = read.bytes;
    out->code = shared::Code::Ok;
    out->detail.clear();
    out->tables = std::move(result.tables);
    out->entry_count = result.entry_count;
    out->archive_bytes = result.archive_bytes;
    return shared::Code::Ok;
}

} // namespace bybus::translation
