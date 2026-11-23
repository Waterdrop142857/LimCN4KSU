// service/generation_store.h
// Responsibility: own generations/<id> as an immutable directory set and the
// root-level `active` pointer. Publishing is: build into generations/.tmp-*,
// fsync, rename into place, then rewrite `active` through active.tmp and a
// rename (task contract §10, 10-port-plan.md §4).
// Nothing here pairs text or parses archives; it only moves and validates
// directories and reads the manifest that build_generation() wrote.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "service/paths.h"
#include "shared/status.h"
#include "translation/snapshot.h"

namespace bybus::service {

struct ActiveSnapshot {
    std::string generation;
    bool present = false;
    bool valid = false;
    int64_t built_at_millis = 0;
    uint32_t table_count = 0;
    std::string source_id;
    std::string archive_sha256;
    std::string font_sha256;
    shared::Code code = shared::Code::Ok;  // Ok when valid, else the reason
};

class GenerationStore {
public:
    explicit GenerationStore(ServicePaths paths);

    // Creates generations/.tmp-<random> and returns it. The caller must remove
    // it on failure.
    shared::Code create_temp(std::string *temp_dir) const;

    // Makes `temp_dir` durable and renames it to generations/<generation>,
    // then points `active` at it. When the same generation already exists the
    // temp directory is dropped and the existing one is kept.
    shared::Code publish(const std::string &temp_dir, const std::string &generation,
                         bool activate = true) const;

    // Reads the `active` pointer; `generation` is empty when absent.
    shared::Code read_active(std::string *generation) const;
    // Rewrites `active` through active.tmp and a rename.
    shared::Code write_active(const std::string &generation) const;
    shared::Code clear_active() const;

    // Reads and validates the active manifest. Never fails: `out->code` says
    // what happened (SnapshotInvalid / IoError / Ok).
    ActiveSnapshot inspect_active() const;

    // Removes generations/.tmp-* left by an interrupted publish.
    shared::Code cleanup_temps() const;

    // Keeps `active` plus the newest kKeepGenerations other generations.
    // Returns the number of removed directories.
    shared::Code prune(uint32_t *removed, const std::string &protected_generation = {}) const;

    // Generation directory names, newest first. Bounded, temp dirs excluded.
    std::vector<std::string> list() const;
    bool exists(const std::string &generation) const;
    std::string dir_of(const std::string &generation) const;
    int64_t built_at_millis(const std::string &generation) const;

private:
    ServicePaths paths_;
};

} // namespace bybus::service
