// Archive reader for the two formats accepted by the importer: ZIP and
// 7z carrying JSON text packs. Format detection uses signatures only; entry
// names are delivered raw so the importer can apply its own path policy.
#pragma once

#include <cstdint>
#include <string>

#include "shared/sha256.h"
#include "shared/status.h"

namespace bybus::translation {

enum class ArchiveFormat { Unknown, Zip, SevenZ };

struct ArchiveEntry {
    std::string raw_name;
    // Stored (uncompressed) size when the container declares it, 0 otherwise.
    uint64_t declared_bytes = 0;
    bool directory = false;
};

// Delivery order is the container order. Skipped entries are not extracted.
class ArchiveVisitor {
public:
    virtual ~ArchiveVisitor() = default;
    // Return false to abort the whole read with archive_invalid.
    virtual bool on_entry(const ArchiveEntry &entry, bool *skip) = 0;
    virtual bool on_data(const char *data, size_t size) = 0;
    virtual bool on_entry_end() = 0;
};

struct ArchiveResult {
    shared::Code code = shared::Code::Ok;
    std::string detail;
    uint32_t entries = 0;
    uint64_t bytes = 0;
};

ArchiveFormat detect_format(const std::string &path);

// Reads entries without resource content quotas. Never writes files.
// - unreadable / non-regular / OS failure -> io_error
// - malformed container, unsupported coder, CRC or size mismatch -> archive_invalid
// Path safety and duplicate-target rejection belong to the importer, which sees
// the decoded names; this layer only reports what the container declared.
ArchiveResult read_archive(const std::string &path, ArchiveVisitor &visitor);

} // namespace bybus::translation
