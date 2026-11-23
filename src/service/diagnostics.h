// service/diagnostics.h
// Responsibility: two bounded observability channels.
//  - Logger: /data/adb/bybus/logs/bybusd.log, rotated once to .1 at 1 MiB.
//  - DiagnosticsLog: /data/adb/bybus/logs/diagnostics.log, the structured
//    200-entry ring the `diagnostics` command exposes (20-protocol.md §3.4).
// Neither channel may carry archive text, translation text, account data or a
// device identifier (02-code-quality-standard.md §4). Source: task contract
// §6, §12, §14.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "shared/status.h"

namespace bybus::service {

class Logger {
public:
    Logger() = default;

    // Creates the log file if needed and rotates an oversized file first.
    shared::Code open(const std::string &path);

    void info(const std::string &message);
    void error(const std::string &message);

    bool enabled() const { return !path_.empty(); }

private:
    void write_line(const char *level, const std::string &message);
    void rotate_if_needed();

    std::mutex mu_;
    std::string path_;
};

struct DiagnosticEntry {
    int64_t at_millis = 0;
    std::string stage;
    std::string code;
    uint64_t count = 0;
};

// Structured, bounded record of build stages. Each entry is one JSON object
// {"atMillis":..,"stage":"..","code":"..","count":..}; the file is rewritten
// on every append so it can never grow past the entry budget.
class DiagnosticsLog {
public:
    DiagnosticsLog() = default;

    shared::Code load(const std::string &path);

    // Appends one entry. `stage` is a fixed token ("pairing", "font", ...);
    // `code` is a protocol code, or a fixed local token such as
    // "font_unverified" when the contract asks for a diagnostic that has no
    // protocol code.
    void record(const std::string &stage, shared::Code code, uint64_t count = 0);
    void record_text(const std::string &stage, const std::string &code, uint64_t count = 0);

    std::vector<DiagnosticEntry> entries() const;

private:
    void persist_locked();

    mutable std::mutex mu_;
    std::string path_;
    std::vector<DiagnosticEntry> entries_;
};

} // namespace bybus::service
