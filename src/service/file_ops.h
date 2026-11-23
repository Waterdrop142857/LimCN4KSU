// service/file_ops.h
// Responsibility: the two streaming file operations the service needs beyond
// shared/fs.h — digest a file without loading it, and copy a file while
// digesting it. Bounded, no allocation proportional to the file size.
#pragma once

#include <cstdint>
#include <string>

#include "shared/status.h"

namespace bybus::service {

// Reads the whole file in bounded chunks and reports its SHA-256 and length.
shared::Code hash_file(const std::string &path, std::string *sha256_hex, uint64_t *bytes);

// Copies `source` to `target` (0600, truncated) while hashing it. Fails with
// archive_limit when the source is larger than `max_bytes`.
shared::Code copy_file_hashed(const std::string &source, const std::string &target,
                              uint64_t max_bytes, std::string *sha256_hex, uint64_t *bytes);

} // namespace bybus::service
