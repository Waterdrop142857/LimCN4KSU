// service/source_registry.h
// Responsibility: the source table and the small piece of cross-run state
// (/data/adb/bybus/state.json) that 20-protocol.md §3.1/§3.2 and
// 10-port-plan.md §1 require: which source is selected and what the last build
// did. Source records live one per file in sources/<id>.json.
// No archive parsing, no jobs, no network here.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "service/paths.h"
#include "shared/status.h"

namespace bybus::service {

struct SourceRecord {
    std::string id;
    std::string name;
    std::string kind;      // "official" | "local"
    std::string revision;  // archive digest, 64 lowercase hex when known
    std::string archive_sha256;
    uint32_t table_count = 0;
    int64_t updated_at_millis = 0;
};

class SourceRegistry {
public:
    struct BuildStats {
        std::string last_generation;
        std::string last_code = "ok";
        uint32_t generations = 0;
        uint32_t discarded = 0;
    };

    explicit SourceRegistry(const ServicePaths &paths);

    // Reads state.json (if any) and sources/*.json. Missing files are not an
    // error: a first run starts with an empty table.
    shared::Code load();

    // Creates the built-in `zero` record when absent and selects it when the
    // stored selection no longer exists.
    shared::Code ensure_default();

    std::vector<SourceRecord> list() const;
    bool find(const std::string &id, SourceRecord *out) const;
    std::string selected_id() const;
    SourceRecord selected() const;

    shared::Code select(const std::string &id);
    shared::Code remove(const std::string &id, std::vector<SourceRecord> *remaining);
    shared::Code upsert(const SourceRecord &record, bool make_selected);

    BuildStats build_stats() const;
    shared::Code record_build(const std::string &generation, shared::Code code, uint32_t discarded);

    // archives/<sha256>.pack
    std::string archive_path(const std::string &sha256) const;

private:
    shared::Code persist_state();
    shared::Code persist_source(const SourceRecord &record);
    std::string state_json_locked() const;
    void store_locked(const SourceRecord &record);

    mutable std::mutex mu_;
    std::mutex persist_mu_;
    std::string sources_dir_;
    std::string state_file_;
    std::string archives_dir_;
    std::vector<SourceRecord> sources_;
    std::string selected_ = kDefaultSourceId;
    BuildStats stats_;
};

} // namespace bybus::service
