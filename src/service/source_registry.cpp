// service/source_registry.cpp
// Responsibility: implementation of the source table and state.json handling.
// Writes are "build the JSON from a snapshot under the state lock, then write
// outside the lock" so no file I/O ever runs while a shared structure is
// locked (task contract §13).
#include "service/source_registry.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "shared/fs.h"
#include "shared/json.h"
#include "shared/sha256.h"

namespace bybus::service {
namespace {

shared::JsonPtr source_json(const SourceRecord &record) {
    shared::JsonPtr root = shared::Json::object();
    root->put_int("schema", 1);
    root->put_string("id", record.id);
    root->put_string("name", record.name);
    root->put_string("kind", record.kind);
    root->put_string("revision", record.revision);
    root->put_int("tableCount", static_cast<int64_t>(record.table_count));
    root->put_string("archiveSha256", record.archive_sha256);
    root->put_int("updatedAtMillis", record.updated_at_millis);
    return root;
}

bool parse_source(const std::string &text, const std::string &expected_id, SourceRecord *out) {
    const shared::ParseOutcome parsed = shared::parse(text);
    if (!parsed.ok || !parsed.value || !parsed.value->is_object()) return false;
    const shared::JsonPtr root = parsed.value;
    const shared::JsonPtr id = root->opt("id");
    const shared::JsonPtr name = root->opt("name");
    const shared::JsonPtr kind = root->opt("kind");
    if (!id || !id->is_string() || id->as_string() != expected_id) return false;
    if (!name || !name->is_string() || name->as_string().size() > 256) return false;
    if (!kind || !kind->is_string()) return false;
    if (kind->as_string() != "official" && kind->as_string() != "local") return false;
    SourceRecord record;
    record.id = id->as_string();
    record.name = name->as_string();
    record.kind = kind->as_string();
    const shared::JsonPtr revision = root->opt("revision");
    if (revision && revision->is_string() && shared::is_sha256_hex(revision->as_string())) {
        record.revision = revision->as_string();
    }
    const shared::JsonPtr archive = root->opt("archiveSha256");
    if (archive && archive->is_string() && shared::is_sha256_hex(archive->as_string())) {
        record.archive_sha256 = archive->as_string();
    }
    if (record.revision.empty()) record.revision = record.archive_sha256;
    const shared::JsonPtr tables = root->opt("tableCount");
    if (tables && tables->is_integral() && tables->as_int() >= 0 &&
        static_cast<uint64_t>(tables->as_int()) <= UINT32_MAX) {
        record.table_count = static_cast<uint32_t>(tables->as_int());
    }
    const shared::JsonPtr updated = root->opt("updatedAtMillis");
    if (updated && updated->is_integral() && updated->as_int() >= 0) {
        record.updated_at_millis = updated->as_int();
    }
    *out = std::move(record);
    return true;
}

} // namespace

SourceRegistry::SourceRegistry(const ServicePaths &paths)
    : sources_dir_(paths.sources()),
      state_file_(paths.state_file()),
      archives_dir_(paths.archives()) {}

shared::Code SourceRegistry::load() {
    std::string state_text;
    std::string selected = kDefaultSourceId;
    BuildStats stats;
    const shared::Code read = shared::read_file(state_file_, kMaxStateBytes, &state_text);
    if (read == shared::Code::Ok) {
        const shared::ParseOutcome parsed = shared::parse(state_text);
        if (parsed.ok && parsed.value && parsed.value->is_object()) {
            const shared::JsonPtr root = parsed.value;
            const shared::JsonPtr id = root->opt("selectedId");
            if (id && id->is_string() && valid_source_id(id->as_string())) {
                selected = id->as_string();
            }
            const shared::JsonPtr build = root->opt("build");
            if (build && build->is_object()) {
                const shared::JsonPtr last_gen = build->opt("lastGeneration");
                const shared::JsonPtr last_code = build->opt("lastCode");
                const shared::JsonPtr count = build->opt("generations");
                const shared::JsonPtr discarded = build->opt("discarded");
                if (last_gen && last_gen->is_string() && shared::is_sha256_hex(last_gen->as_string())) {
                    stats.last_generation = last_gen->as_string();
                }
                if (last_code && last_code->is_string() && !last_code->as_string().empty()) {
                    stats.last_code = last_code->as_string();
                }
                if (count && count->is_integral() && count->as_int() >= 0) {
                    stats.generations = static_cast<uint32_t>(count->as_int());
                }
                if (discarded && discarded->is_integral() && discarded->as_int() >= 0) {
                    stats.discarded = static_cast<uint32_t>(discarded->as_int());
                }
            }
        }
    } else if (read != shared::Code::GameMissing) {
        return read;
    }

    std::vector<std::string> names;
    const shared::Code listed = shared::list_dir(sources_dir_, kMaxSourceFiles, &names);
    if (listed != shared::Code::Ok && listed != shared::Code::GameMissing) return listed;

    std::vector<SourceRecord> records;
    for (const std::string &name : names) {
        if (name.size() < 6) continue;
        if (name.compare(name.size() - 5, 5, ".json") != 0) continue;
        const std::string id = name.substr(0, name.size() - 5);
        if (!valid_source_id(id)) continue;
        std::string text;
        const shared::Code source_read =
            shared::read_file(sources_dir_ + "/" + name, kMaxJobFileBytes, &text);
        if (source_read != shared::Code::Ok) continue;
        SourceRecord record;
        if (parse_source(text, id, &record)) records.push_back(std::move(record));
    }

    std::lock_guard<std::mutex> guard(mu_);
    sources_ = std::move(records);
    stats_ = stats;
    selected_ = selected;
    return shared::Code::Ok;
}

shared::Code SourceRegistry::ensure_default() {
    SourceRecord record;
    record.id = kDefaultSourceId;
    record.name = kDefaultSourceName;
    record.kind = "official";
    bool have_default = false;
    {
        std::lock_guard<std::mutex> guard(mu_);
        for (const SourceRecord &item : sources_) {
            if (item.id == kDefaultSourceId) have_default = true;
        }
    }
    if (!have_default) {
        const shared::Code wrote = upsert(record, false);
        if (wrote != shared::Code::Ok) return wrote;
    }
    bool selected_exists = false;
    {
        std::lock_guard<std::mutex> guard(mu_);
        for (const SourceRecord &item : sources_) {
            if (item.id == selected_) selected_exists = true;
        }
        if (!selected_exists) selected_ = kDefaultSourceId;
    }
    return persist_state();
}

std::vector<SourceRecord> SourceRegistry::list() const {
    std::lock_guard<std::mutex> guard(mu_);
    return sources_;
}

bool SourceRegistry::find(const std::string &id, SourceRecord *out) const {
    std::lock_guard<std::mutex> guard(mu_);
    for (const SourceRecord &item : sources_) {
        if (item.id == id) {
            if (out) *out = item;
            return true;
        }
    }
    return false;
}

std::string SourceRegistry::selected_id() const {
    std::lock_guard<std::mutex> guard(mu_);
    return selected_;
}

SourceRecord SourceRegistry::selected() const {
    std::lock_guard<std::mutex> guard(mu_);
    for (const SourceRecord &item : sources_) {
        if (item.id == selected_) return item;
    }
    SourceRecord fallback;
    fallback.id = selected_;
    fallback.kind = "official";
    return fallback;
}

shared::Code SourceRegistry::select(const std::string &id) {
    SourceRecord record;
    if (!find(id, &record)) return shared::Code::SourceMissing;
    {
        std::lock_guard<std::mutex> guard(mu_);
        selected_ = id;
    }
    return persist_state();
}

shared::Code SourceRegistry::remove(const std::string &id, std::vector<SourceRecord> *remaining) {
    if (id == kDefaultSourceId) return shared::Code::InvalidArgument;
    if (!valid_source_id(id)) return shared::Code::InvalidArgument;
    if (!find(id, nullptr)) return shared::Code::SourceMissing;
    if (::unlink((sources_dir_ + "/" + id + ".json").c_str()) != 0 && errno != ENOENT) {
        return shared::Code::IoError;
    }
    {
        std::lock_guard<std::mutex> guard(mu_);
        for (size_t index = 0; index < sources_.size(); ++index) {
            if (sources_[index].id == id) {
                sources_.erase(sources_.begin() + static_cast<std::ptrdiff_t>(index));
                break;
            }
        }
        if (selected_ == id) selected_ = kDefaultSourceId;
        if (remaining) *remaining = sources_;
    }
    return persist_state();
}

shared::Code SourceRegistry::upsert(const SourceRecord &record, bool make_selected) {
    if (!valid_source_id(record.id)) return shared::Code::InvalidArgument;
    if (record.name.size() > 256) return shared::Code::InvalidArgument;
    if (record.kind != "official" && record.kind != "local") return shared::Code::InvalidArgument;
    {
        std::lock_guard<std::mutex> guard(mu_);
        store_locked(record);
        if (make_selected) selected_ = record.id;
    }
    const shared::Code wrote = persist_source(record);
    if (wrote != shared::Code::Ok) return wrote;
    return persist_state();
}

SourceRegistry::BuildStats SourceRegistry::build_stats() const {
    std::lock_guard<std::mutex> guard(mu_);
    return stats_;
}

shared::Code SourceRegistry::record_build(const std::string &generation, shared::Code code,
                                          uint32_t discarded) {
    {
        std::lock_guard<std::mutex> guard(mu_);
        if (!generation.empty()) {
            stats_.last_generation = generation;
            ++stats_.generations;
        }
        stats_.last_code = shared::code_text(code);
        stats_.discarded += discarded;
    }
    return persist_state();
}

std::string SourceRegistry::archive_path(const std::string &sha256) const {
    return archives_dir_ + "/" + sha256 + ".pack";
}

void SourceRegistry::store_locked(const SourceRecord &record) {
    for (SourceRecord &item : sources_) {
        if (item.id == record.id) {
            item = record;
            return;
        }
    }
    sources_.push_back(record);
    std::sort(sources_.begin(), sources_.end(),
              [](const SourceRecord &left, const SourceRecord &right) { return left.id < right.id; });
}

std::string SourceRegistry::state_json_locked() const {
    shared::JsonPtr root = shared::Json::object();
    root->put_int("schema", 1);
    root->put_string("selectedId", selected_);
    shared::JsonPtr build = root->put_object("build");
    build->put_string("lastGeneration", stats_.last_generation);
    build->put_string("lastCode", stats_.last_code);
    build->put_int("generations", static_cast<int64_t>(stats_.generations));
    build->put_int("discarded", static_cast<int64_t>(stats_.discarded));
    return root->dump();
}

shared::Code SourceRegistry::persist_state() {
    std::lock_guard<std::mutex> guard(persist_mu_);
    std::string content;
    {
        std::lock_guard<std::mutex> state_guard(mu_);
        content = state_json_locked();
    }
    return write_atomic(state_file_, content);
}

shared::Code SourceRegistry::persist_source(const SourceRecord &record) {
    const std::string content = source_json(record)->dump();
    return write_atomic(sources_dir_ + "/" + record.id + ".json", content);
}

} // namespace bybus::service
