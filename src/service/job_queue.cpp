#include "service/job_queue.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "service/file_ops.h"
#include "service/game_scan.h"
#include "service/source_fetch.h"
#include "shared/fs.h"
#include "shared/json.h"
#include "shared/sha256.h"
#include "translation/snapshot_builder.h"

namespace bybus::service {
namespace {

constexpr size_t kMaxPackCacheEntries = 2;

std::string state_folder(JobState state) {
    switch (state) {
        case JobState::Queued: return "queued";
        case JobState::Fetching: return "fetching";
        case JobState::Validating: return "validating";
        case JobState::Pairing: return "pairing";
        case JobState::Publishing: return "publishing";
        case JobState::Done: return "done";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return "queued";
}

bool state_from_text(const std::string &text, JobState *out) {
    static const std::pair<const char *, JobState> kStates[] = {
        {"queued", JobState::Queued},         {"fetching", JobState::Fetching},
        {"validating", JobState::Validating}, {"pairing", JobState::Pairing},
        {"publishing", JobState::Publishing}, {"done", JobState::Done},
        {"failed", JobState::Failed},         {"cancelled", JobState::Cancelled},
    };
    for (const auto &entry : kStates) {
        if (text == entry.first) {
            *out = entry.second;
            return true;
        }
    }
    return false;
}

} // namespace

const char *job_state_text(JobState state) {
    switch (state) {
        case JobState::Queued: return "queued";
        case JobState::Fetching: return "fetching";
        case JobState::Validating: return "validating";
        case JobState::Pairing: return "pairing";
        case JobState::Publishing: return "publishing";
        case JobState::Done: return "done";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return "queued";
}

bool terminal(JobState state) {
    return state == JobState::Done || state == JobState::Failed || state == JobState::Cancelled;
}

JobQueue::JobQueue(const ServicePaths &paths, Logger *log, DiagnosticsLog *diagnostics,
                   SourceRegistry *registry, GenerationStore *generations)
    : paths_(paths), log_(log), diagnostics_(diagnostics), registry_(registry),
      generations_(generations) {}

JobQueue::~JobQueue() { stop(); }

void JobQueue::start() {
    if (running_.exchange(true)) return;
    stopping_.store(false);
    worker_ = std::thread([this]() { worker_loop(); });
}

void JobQueue::stop() {
    stopping_.store(true);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

std::string JobQueue::dedupe_key(const JobSpec &spec) const {
    return spec.kind + "|" + spec.mode + "|" + spec.url + "|" + spec.archive_path + "|" +
           spec.local_path + "|" + spec.expected_sha256 + "|" + spec.source_id +
           (spec.activate ? "|1" : "|0");
}

shared::Code JobQueue::submit(const JobSpec &spec, std::string *job_id) {
    if (!job_id) return shared::Code::InvalidArgument;
    std::lock_guard<std::mutex> guard(mu_);
    const std::string key = dedupe_key(spec);
    const auto active = dedupe_.find(key);
    if (active != dedupe_.end()) {
        *job_id = active->second;
        return shared::Code::Ok;
    }
    JobView view;
    view.id = "j-" + random_hex(16);
    view.kind = spec.kind;
    view.state = JobState::Queued;
    view.stage = "queued";
    view.started_at_millis = now_millis();
    view.source_id = spec.source_id;
    if (persist(view) != shared::Code::Ok) return shared::Code::IoError;
    jobs_.push_back(view);
    dedupe_[key] = view.id;
    pending_.emplace_back(spec, view);
    *job_id = view.id;
    wake_.notify_all();
    return shared::Code::Ok;
}

bool JobQueue::view(const std::string &job_id, JobView *out) const {
    if (!out) return false;
    std::lock_guard<std::mutex> guard(mu_);
    for (const JobView &view : jobs_) {
        if (view.id == job_id) {
            *out = view;
            return true;
        }
    }
    // Historical jobs are read on demand. Appending one here would make an old
    // status query change latest_id(), which drives the WebUI overview.
    return load_job(job_id, out);
}

shared::Code JobQueue::cancel(const std::string &job_id) {
    std::lock_guard<std::mutex> guard(mu_);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->second.id != job_id) continue;
        it->second.state = JobState::Cancelled;
        it->second.error_code = "job_cancelled";
        it->second.stage = "cancelled";
        for (JobView &view : jobs_) {
            if (view.id == job_id) view = it->second;
        }
        persist(it->second);
        pending_.erase(it);
        for (auto entry = dedupe_.begin(); entry != dedupe_.end();) {
            if (entry->second == job_id) entry = dedupe_.erase(entry);
            else ++entry;
        }
        return shared::Code::Ok;
    }
    for (const JobView &view : jobs_) {
        if (view.id != job_id) continue;
        if (terminal(view.state)) return shared::Code::JobCancelled;
        if (view.state == JobState::Publishing) return shared::Code::SnapshotBusy;
        cancels_[job_id] = "1";
        return shared::Code::Ok;
    }
    JobView historical;
    if (load_job(job_id, &historical) && terminal(historical.state)) {
        return shared::Code::JobCancelled;
    }
    return shared::Code::JobMissing;
}

std::string JobQueue::latest_id() const {
    std::lock_guard<std::mutex> guard(mu_);
    if (jobs_.empty()) return std::string();
    return jobs_.back().id;
}

std::string JobQueue::running_id() const {
    std::lock_guard<std::mutex> guard(mu_);
    for (const JobView &view : jobs_) {
        if (!terminal(view.state)) return view.id;
    }
    return std::string();
}

shared::Code JobQueue::persist(const JobView &view) const {
    shared::JsonPtr root = shared::Json::object();
    root->put_string("id", view.id);
    root->put_string("state", state_folder(view.state));
    root->put_string("stage", view.stage);
    root->put_string("errorCode", view.error_code);
    root->put_string("detail", view.detail);
    root->put_int("done", view.done);
    root->put_int("total", view.total);
    root->put_int("startedAtMillis", view.started_at_millis);
    root->put_string("sourceId", view.source_id);
    root->put_string("generation", view.generation);
    root->put_string("kind", view.kind);
    return write_atomic(paths_.job_file(view.id), root->dump());
}

bool JobQueue::load_job(const std::string &job_id, JobView *out) const {
    if (!valid_job_id(job_id)) return false;
    std::string text;
    if (shared::read_file(paths_.job_file(job_id), kMaxJobFileBytes, &text) != shared::Code::Ok) {
        return false;
    }
    const shared::ParseOutcome parsed = shared::parse(text);
    if (!parsed.ok || !parsed.value || !parsed.value->is_object()) return false;
    const shared::JsonPtr root = parsed.value;
    const shared::JsonPtr id = root->opt("id");
    const shared::JsonPtr state = root->opt("state");
    if (!id || !id->is_string() || id->as_string() != job_id || !state || !state->is_string()) {
        return false;
    }
    JobView view;
    view.id = job_id;
    if (!state_from_text(state->as_string(), &view.state)) return false;
    const shared::JsonPtr stage = root->opt("stage");
    if (stage && stage->is_string()) view.stage = stage->as_string();
    const shared::JsonPtr error = root->opt("errorCode");
    if (error && error->is_string()) view.error_code = error->as_string();
    const shared::JsonPtr detail = root->opt("detail");
    if (detail && detail->is_string()) view.detail = detail->as_string();
    const shared::JsonPtr done = root->opt("done");
    if (done && done->is_integral()) view.done = static_cast<uint32_t>(done->as_int());
    const shared::JsonPtr total = root->opt("total");
    if (total && total->is_integral()) view.total = static_cast<uint32_t>(total->as_int());
    const shared::JsonPtr started = root->opt("startedAtMillis");
    if (started && started->is_integral()) view.started_at_millis = started->as_int();
    const shared::JsonPtr source = root->opt("sourceId");
    if (source && source->is_string()) view.source_id = source->as_string();
    const shared::JsonPtr generation = root->opt("generation");
    if (generation && generation->is_string()) view.generation = generation->as_string();
    const shared::JsonPtr kind = root->opt("kind");
    if (kind && kind->is_string()) view.kind = kind->as_string();
    *out = view;
    return true;
}

void JobQueue::publish_view(const JobView &view) {
    std::lock_guard<std::mutex> guard(mu_);
    for (JobView &slot : jobs_) {
        if (slot.id == view.id) {
            slot = view;
            break;
        }
    }
    persist(view);
}

void JobQueue::recover() {
    std::vector<std::string> names;
    if (shared::list_dir(paths_.jobs(), std::numeric_limits<uint32_t>::max(), &names) !=
        shared::Code::Ok) return;
    std::vector<JobView> recovered;
    JobView latest;
    bool have_latest = false;
    for (const std::string &name : names) {
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
        const std::string id = name.substr(0, name.size() - 5);
        JobView view;
        if (!load_job(id, &view)) continue;
        if (!terminal(view.state)) {
            view.state = JobState::Failed;
            view.stage = "failed";
            view.error_code = "io_error";
            (void)persist(view);
            recovered.push_back(view);
        }
        if (!have_latest || view.started_at_millis > latest.started_at_millis ||
            (view.started_at_millis == latest.started_at_millis && view.id > latest.id)) {
            latest = view;
            have_latest = true;
        }
    }
    if (have_latest && std::none_of(recovered.begin(), recovered.end(),
                                    [&latest](const JobView &view) { return view.id == latest.id; })) {
        recovered.push_back(latest);
    }
    std::sort(recovered.begin(), recovered.end(), [](const JobView &left, const JobView &right) {
        if (left.started_at_millis != right.started_at_millis)
            return left.started_at_millis < right.started_at_millis;
        return left.id < right.id;
    });
    {
        std::lock_guard<std::mutex> guard(mu_);
        jobs_ = std::move(recovered);
    }
    if (log_) log_->info("job recovery complete");
}

void JobQueue::worker_loop() {
    while (!stopping_.load()) {
        std::pair<JobSpec, JobView> entry;
        {
            std::unique_lock<std::mutex> lock(mu_);
            wake_.wait_for(lock, std::chrono::milliseconds(500),
                           [this]() { return !pending_.empty() || stopping_.load(); });
            if (stopping_.load()) break;
            if (pending_.empty()) continue;
            entry = pending_.front();
            pending_.erase(pending_.begin());
        }
        JobView view = entry.second;
        const shared::Code code = execute(&view, entry.first);
        view.state = code == shared::Code::Ok ? JobState::Done
                     : code == shared::Code::JobCancelled ? JobState::Cancelled
                                                          : JobState::Failed;
        if (code != shared::Code::Ok) {
            view.error_code = shared::code_text(code);
            if (view.stage != "publishing") view.stage = "failed";
        }
        {
            std::lock_guard<std::mutex> guard(mu_);
            const std::string key = dedupe_key(entry.first);
            const auto found = dedupe_.find(key);
            if (found != dedupe_.end() && found->second == view.id) dedupe_.erase(found);
            cancels_.erase(view.id);
        }
        publish_view(view);
        if (log_) log_->info("job " + view.id + " -> " + state_folder(view.state));
    }
}

shared::Code JobQueue::resolve_archive(JobView *view, const JobSpec &spec,
                                        std::string *archive_path, std::string *display_name,
                                        std::string *expected_sha256) {
    if (!spec.archive_path.empty()) {
        *archive_path = spec.archive_path;
        *display_name = spec.display_name;
        return shared::Code::Ok;
    }

    // mode=path: the daemon runs as root, so the user-supplied file is copied
    // straight from disk instead of being uploaded through the WebView.
    if (spec.mode == "path") {
        view->state = JobState::Fetching;
        view->stage = "fetching";
        publish_view(*view);
        if (spec.local_path.empty()) return shared::Code::InvalidArgument;
        const std::string work = paths_.imports() + "/.fetch-" + view->id;
        if (shared::make_dirs(work, 0700) != shared::Code::Ok) return shared::Code::IoError;
        const std::string target = work + "/source.pack";
        std::string digest;
        uint64_t bytes = 0;
        const shared::Code copied =
            copy_file_hashed(spec.local_path, target, kMaxArchiveBytes, &digest, &bytes);
        if (copied != shared::Code::Ok) {
            shared::remove_tree(work);
            return copied;
        }
        *archive_path = target;
        const size_t slash = spec.local_path.find_last_of('/');
        *display_name = slash == std::string::npos ? spec.local_path
                                                   : spec.local_path.substr(slash + 1);
        return shared::Code::Ok;
    }
    view->state = JobState::Fetching;
    view->stage = "fetching";
    publish_view(*view);
    FetchOutcome outcome;
    shared::Code code = shared::Code::UnsupportedRuntime;
    const std::string work = paths_.imports() + "/.fetch-" + view->id;
    if (shared::make_dirs(work, 0700) != shared::Code::Ok) return shared::Code::IoError;
    if (spec.mode == "release") {
        code = spec.url.empty() ? fetch_release(paths_, work, &outcome)
                                : fetch_url(paths_, spec.url, work, &outcome);
    } else if (spec.mode == "inbox") {
        code = fetch_inbox(paths_, &outcome);
    } else if (spec.mode == "url") {
        code = fetch_url(paths_, spec.url, work, &outcome);
    } else {
        code = shared::Code::InvalidArgument;
    }
    if (code != shared::Code::Ok) {
        shared::remove_tree(work);
        return code;
    }
    *archive_path = outcome.archive_path;
    *display_name = outcome.display_name;
    *expected_sha256 = outcome.expected_sha256;
    return shared::Code::Ok;
}

shared::Code JobQueue::import_archive_cached(const std::string &archive_path,
                                             const std::string &expected_sha256,
                                             std::string *sha256,
                                             std::vector<translation::ImportedTable> *tables,
                                             std::string *detail) {
    if (hash_file(archive_path, sha256, nullptr) != shared::Code::Ok) return shared::Code::IoError;
    if (!expected_sha256.empty() && *sha256 != expected_sha256) {
        return shared::Code::ArchiveInvalid;
    }
    {
        std::lock_guard<std::mutex> guard(cache_mu_);
        for (const auto &entry : pack_cache_) {
            if (entry.first != *sha256) continue;
            *tables = entry.second;
            return shared::Code::Ok;
        }
    }
    translation::ImportResult imported;
    const shared::Code code = translation::import_archive(archive_path, &imported);
    if (code != shared::Code::Ok) {
        if (detail) *detail = imported.detail;
        return code;
    }
    if (imported.archive_sha256.empty()) imported.archive_sha256 = *sha256;
    *tables = imported.tables;
    std::lock_guard<std::mutex> guard(cache_mu_);
    while (pack_cache_.size() >= kMaxPackCacheEntries) pack_cache_.erase(pack_cache_.begin());
    pack_cache_.emplace_back(*sha256, *tables);
    return shared::Code::Ok;
}

shared::Code JobQueue::build_and_publish(JobView *view, const JobSpec &spec,
                                         const std::string &archive_sha256,
                                         const std::vector<translation::ImportedTable> &tables) {
    GameInstall install;
    if (probe_install(&install, log_) != shared::Code::Ok || !install.installed) {
        return shared::Code::GameMissing;
    }
    GameTextScan texts;
    const shared::Code scanned = scan_game_texts(install, true, &texts);
    if (scanned != shared::Code::Ok) return scanned;
    if (texts.tables.empty()) return shared::Code::GameMissingText;

    std::string temp;
    if (generations_->create_temp(&temp) != shared::Code::Ok) return shared::Code::IoError;
    translation::BuildInputs inputs;
    inputs.work_dir = temp;
    inputs.source_id = spec.source_id.empty() ? registry_->selected_id() : spec.source_id;
    inputs.archive_sha256 = archive_sha256;
    inputs.game_version_code = install.version_code;
    inputs.game_install_identity = install.install_identity;
    inputs.package_name = install.package_name;
    inputs.game_apk_paths = install.apk_paths;
    inputs.sdk = install.sdk;
    inputs.page_size = install.page_size;
    inputs.abi = install.abi;
    inputs.signer_sha256 = install.signer_sha256;
    inputs.application_class = install.application_class;
    inputs.declares_protection = install.declares_protection;
    inputs.native_alignment_ok = install.native_alignment.ok;
    inputs.native_alignment_error = install.native_alignment.error;
    inputs.font_source_path = paths_.font_source();
    inputs.originals = std::move(texts.tables);
    inputs.pack = tables;

    view->state = JobState::Pairing;
    view->stage = "pairing";
    view->total = static_cast<uint32_t>(inputs.originals.size());
    publish_view(*view);
    const translation::BuildOutcome built = translation::build_generation(inputs);
    view->done = built.tables;
    if (built.code != shared::Code::Ok) {
        view->detail = built.detail;
        shared::remove_tree(temp);
        if (diagnostics_) diagnostics_->record("pairing", built.code, built.tables);
        return built.code;
    }
    bool cancel_before_publish = false;
    {
        std::lock_guard<std::mutex> guard(mu_);
        cancel_before_publish = cancels_.find(view->id) != cancels_.end();
    }
    if (cancel_before_publish) {
        shared::remove_tree(temp);
        return shared::Code::JobCancelled;
    }
    view->state = JobState::Publishing;
    view->stage = "publishing";
    publish_view(*view);
    const shared::Code published = generations_->publish(temp, built.generation, spec.activate);
    if (published != shared::Code::Ok) {
        shared::remove_tree(temp);
        return published;
    }
    view->generation = built.generation;
    if (!built.profile_present) {
        // Published, but the runtime will not patch this build. Reported as a
        // completed job with the fixed code so the UI can explain it.
        view->error_code = "unsupported_runtime";
        if (diagnostics_) diagnostics_->record_text("presence", "unsupported_runtime", 0);
    }
    uint32_t removed = 0;
    generations_->prune(&removed, built.generation);
    registry_->record_build(built.generation, shared::Code::Ok, removed);
    return shared::Code::Ok;
}

shared::Code JobQueue::execute(JobView *view, const JobSpec &spec) {
    // Every fetched archive is private to this job. Remove it on every exit,
    // including cancellation and validation failure, after any durable copy.
    struct FetchCleanup {
        std::string path;
        ~FetchCleanup() { if (!path.empty()) (void)shared::remove_tree(path); }
    } cleanup{paths_.imports() + "/.fetch-" + view->id};
    const auto cancelled = [&]() {
        std::lock_guard<std::mutex> guard(mu_);
        return cancels_.find(view->id) != cancels_.end();
    };
    if (cancelled()) return shared::Code::JobCancelled;

    std::string archive_path;
    std::string display_name;
    std::string fetched_sha256;
    const shared::Code resolved = resolve_archive(view, spec, &archive_path, &display_name,
                                                  &fetched_sha256);
    if (resolved != shared::Code::Ok) return resolved;
    if (cancelled()) return shared::Code::JobCancelled;

    view->state = JobState::Validating;
    view->stage = "validating";
    publish_view(*view);
    std::string archive_sha256;
    std::vector<translation::ImportedTable> tables;
    std::string import_detail;
    const std::string &expected_sha256 = spec.expected_sha256.empty()
                                             ? fetched_sha256 : spec.expected_sha256;
    const shared::Code imported = import_archive_cached(archive_path, expected_sha256,
                                                        &archive_sha256, &tables, &import_detail);
    if (imported != shared::Code::Ok) {
        view->detail = import_detail;
        if (diagnostics_) diagnostics_->record_text("validating", import_detail, 0);
        return imported;
    }
    // Keep the verified archive and register (or refresh) the source record.
    SourceRecord record;
    record.id = spec.source_id;
    if (record.id.empty() && spec.mode != "source") {
        record.id = std::string(kLocalSourcePrefix) + archive_sha256.substr(0, 16);
    }
    if (record.id.empty()) record.id = registry_->selected_id();
    record.name = display_name.empty() ? record.id : display_name;
    if (record.name.size() > 64) record.name.resize(64);
    record.kind = record.id == kDefaultSourceId ? "official" : "local";
    record.revision = archive_sha256;
    record.archive_sha256 = archive_sha256;
    record.table_count = static_cast<uint32_t>(tables.size());
    record.updated_at_millis = now_millis();
    if (spec.mode != "source") {
        const std::string target = paths_.archive_file(archive_sha256);
        std::string stored_sha256;
        const bool archive_ready = shared::exists(target) &&
            hash_file(target, &stored_sha256, nullptr) == shared::Code::Ok &&
            stored_sha256 == archive_sha256;
        if (!archive_ready) {
            std::string copied;
            uint64_t bytes = 0;
            const shared::Code copied_code =
                copy_file_hashed(archive_path, target + ".tmp", kMaxArchiveBytes, &copied, &bytes);
            if (copied_code != shared::Code::Ok || copied != archive_sha256) {
                shared::remove_tree(target + ".tmp");
                return copied_code == shared::Code::Ok ? shared::Code::ArchiveInvalid : copied_code;
            }
            const shared::Code published_archive = shared::atomic_replace(target + ".tmp", target);
            if (published_archive != shared::Code::Ok) return published_archive;
        }
        if (registry_->upsert(record, false) != shared::Code::Ok) return shared::Code::IoError;
    }

    if (cancelled()) return shared::Code::JobCancelled;
    JobSpec build_spec = spec;
    build_spec.source_id = record.id;
    const shared::Code built = build_and_publish(view, build_spec, archive_sha256, tables);
    if (built == shared::Code::Ok && spec.activate && registry_->select(record.id) != shared::Code::Ok) {
        return shared::Code::IoError;
    }
    return built;
}

} // namespace bybus::service
