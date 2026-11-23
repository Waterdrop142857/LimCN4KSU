// service/diagnostics.cpp
// Responsibility: implementation of the two bounded observability channels
// declared in diagnostics.h. All file writes go through
// shared::{write_file,atomic_replace}; a record never exceeds the fixed entry
// budget and never contains free-form game or user text.
#include "service/diagnostics.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#include "service/paths.h"
#include "shared/fs.h"
#include "shared/json.h"

namespace bybus::service {
namespace {

std::string timestamp_prefix() {
    const int64_t millis = now_millis();
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%lld ", static_cast<long long>(millis));
    return std::string(buffer);
}

bool parse_entry(const std::string &line, DiagnosticEntry *out) {
    const shared::ParseOutcome parsed = shared::parse(line);
    if (!parsed.ok || !parsed.value || !parsed.value->is_object()) return false;
    const shared::JsonPtr at = parsed.value->opt("atMillis");
    const shared::JsonPtr stage = parsed.value->opt("stage");
    const shared::JsonPtr code = parsed.value->opt("code");
    const shared::JsonPtr count = parsed.value->opt("count");
    if (!at || !at->is_integral()) return false;
    if (!stage || !stage->is_string() || stage->as_string().size() > 32) return false;
    if (!code || !code->is_string() || code->as_string().size() > 32) return false;
    out->at_millis = at->as_int();
    out->stage = stage->as_string();
    out->code = code->as_string();
    out->count = count && count->is_integral() && count->as_int() > 0
                     ? static_cast<uint64_t>(count->as_int())
                     : 0;
    return true;
}

} // namespace

shared::Code Logger::open(const std::string &path) {
    std::lock_guard<std::mutex> guard(mu_);
    path_ = path;
    rotate_if_needed();
    const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        path_.clear();
        return shared::Code::IoError;
    }
    ::close(fd);
    return shared::Code::Ok;
}

void Logger::info(const std::string &message) { write_line("I", message); }

void Logger::error(const std::string &message) { write_line("E", message); }

void Logger::write_line(const char *level, const std::string &message) {
    std::lock_guard<std::mutex> guard(mu_);
    if (path_.empty()) return;
    std::string line = timestamp_prefix();
    line += level;
    line += " ";
    for (const char c : message) {
        // Keep the log single-line and free of control characters.
        line.push_back(static_cast<unsigned char>(c) < 0x20 ? ' ' : c);
    }
    line.push_back('\n');
    rotate_if_needed();
    const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    size_t offset = 0;
    while (offset < line.size()) {
        const ssize_t written = ::write(fd, line.data() + offset, line.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    ::close(fd);
}

void Logger::rotate_if_needed() {
    if (path_.empty()) return;
    struct stat info {};
    if (::stat(path_.c_str(), &info) != 0) return;
    if (static_cast<uint64_t>(info.st_size) <= kMaxLogBytes) return;
    const std::string rotated = path_ + ".1";
    if (::unlink(rotated.c_str()) != 0 && errno != ENOENT) return;
    if (::rename(path_.c_str(), rotated.c_str()) != 0) return;
}

shared::Code DiagnosticsLog::load(const std::string &path) {
    std::lock_guard<std::mutex> guard(mu_);
    path_ = path;
    entries_.clear();
    std::string content;
    const shared::Code read = shared::read_file(path_, kMaxStateBytes, &content);
    if (read != shared::Code::Ok) return read == shared::Code::GameMissing ? shared::Code::Ok
                                                                          : read;
    size_t start = 0;
    while (start < content.size()) {
        const size_t newline = content.find('\n', start);
        const size_t end = newline == std::string::npos ? content.size() : newline;
        if (end > start) {
            DiagnosticEntry entry;
            if (parse_entry(content.substr(start, end - start), &entry)) {
                entries_.push_back(std::move(entry));
            }
        }
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    if (entries_.size() > kMaxDiagnosticsEntries) {
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<std::ptrdiff_t>(
                                               entries_.size() - kMaxDiagnosticsEntries));
    }
    return shared::Code::Ok;
}

void DiagnosticsLog::record(const std::string &stage, shared::Code code, uint64_t count) {
    record_text(stage, shared::code_text(code), count);
}

void DiagnosticsLog::record_text(const std::string &stage, const std::string &code,
                                 uint64_t count) {
    std::lock_guard<std::mutex> guard(mu_);
    DiagnosticEntry entry;
    entry.at_millis = now_millis();
    entry.stage = stage.empty() ? std::string("job") : stage;
    entry.code = code.empty() ? std::string("ok") : code;
    entry.count = count;
    if (entry.stage.size() > 32) entry.stage.resize(32);
    if (entry.code.size() > 32) entry.code.resize(32);
    entries_.push_back(std::move(entry));
    if (entries_.size() > kMaxDiagnosticsEntries) {
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<std::ptrdiff_t>(
                                               entries_.size() - kMaxDiagnosticsEntries));
    }
    persist_locked();
}

std::vector<DiagnosticEntry> DiagnosticsLog::entries() const {
    std::lock_guard<std::mutex> guard(mu_);
    return entries_;
}

void DiagnosticsLog::persist_locked() {
    if (path_.empty()) return;
    std::string content;
    for (const DiagnosticEntry &entry : entries_) {
        shared::JsonPtr row = shared::Json::object();
        row->put_int("atMillis", entry.at_millis);
        row->put_string("stage", entry.stage);
        row->put_string("code", entry.code);
        row->put_int("count", static_cast<int64_t>(entry.count));
        content += row->dump();
        content.push_back('\n');
    }
    (void)write_atomic(path_, content);
}

} // namespace bybus::service
