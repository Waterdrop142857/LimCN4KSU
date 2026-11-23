// service/import_session.h
// Responsibility: the chunked upload session behind `import begin` / `chunk` /
// `commit` / `abort` (20-protocol.md §2, task contract §5). A session owns
// imports/<sessionId>/session.json plus part-<index> files; it never parses an
// archive and never publishes anything.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "service/paths.h"
#include "shared/status.h"

namespace bybus::service {

struct ImportSessionInfo {
    std::string id;
    std::string name;
    std::string sha256;  // expected digest, empty when the client did not send one
    uint64_t total_bytes = 0;
    uint64_t received = 0;
    uint64_t next_index = 0;
    int64_t created_at_millis = 0;
};

class ImportSessions {
public:
    explicit ImportSessions(const ServicePaths &paths);

    shared::Code begin(const std::string &name, uint64_t total_bytes, const std::string &sha256,
                       std::string *session_id);

    // `base64` decodes to at most kImportChunkBytes; `index` must equal the
    // session's next index (0 based, contiguous).
    shared::Code chunk(const std::string &session_id, uint64_t index, const std::string &base64,
                       uint64_t *received, uint64_t *total);

    shared::Code describe(const std::string &session_id, ImportSessionInfo *out) const;

    // Concatenates the parts into `target` (created with 0600) and reports the
    // digest. Fails with archive_invalid when the length is not totalBytes.
    shared::Code assemble(const std::string &session_id, const std::string &target,
                          std::string *sha256_hex, uint64_t *bytes);

    // Same checks as assemble() without writing the concatenation.
    shared::Code verify(const std::string &session_id, std::string *sha256_hex, uint64_t *bytes);

    shared::Code abort(const std::string &session_id);

    // Deletes sessions older than kImportSessionTtlMillis.
    shared::Code cleanup_expired();
    // Deletes every session (startup crash recovery).
    shared::Code cleanup_all();

    std::string dir_of(const std::string &session_id) const;

private:
    shared::Code read_session(const std::string &session_id, ImportSessionInfo *out) const;
    shared::Code write_session(const ImportSessionInfo &info) const;

    ServicePaths paths_;
    mutable std::mutex mu_;
};

} // namespace bybus::service
