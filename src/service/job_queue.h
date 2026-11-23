// service/job_queue.h
// Responsibility: the single background worker behind `update start`,
// `import commit` and `snapshot build` (20-protocol.md §2, §5). Jobs are
// persisted one file per job so a page can close and reattach, run strictly
// serially, and always end in a terminal state.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "service/diagnostics.h"
#include "service/generation_store.h"
#include "service/import_session.h"
#include "service/paths.h"
#include "service/source_registry.h"
#include "shared/status.h"
#include "translation/archive_import.h"

namespace bybus::service {

enum class JobState { Queued, Fetching, Validating, Pairing, Publishing, Done, Failed, Cancelled };

const char *job_state_text(JobState state);
bool terminal(JobState state);

struct JobSpec {
    std::string kind;          // "update" | "build"
    std::string mode;          // release | inbox | url | path | import | source
    std::string url;
    std::string source_id;     // for build / after import
    std::string archive_path;  // already-local archive (import commit)
    std::string local_path;    // user-supplied file for mode=path (daemon runs as root)
    std::string display_name;
    std::string expected_sha256;
    bool activate = true;
};

struct JobView {
    std::string id;
    JobState state = JobState::Queued;
    std::string stage;
    std::string error_code;
    std::string detail;  // importer/build reason, e.g. limit_folder_bytes
    uint32_t done = 0;
    uint32_t total = 0;
    int64_t started_at_millis = 0;
    std::string source_id;
    std::string generation;
    std::string kind;
};

class JobQueue {
public:
    JobQueue(const ServicePaths &paths, Logger *log, DiagnosticsLog *diagnostics,
             SourceRegistry *registry, GenerationStore *generations);
    ~JobQueue();
    JobQueue(const JobQueue &) = delete;
    JobQueue &operator=(const JobQueue &) = delete;

    void start();
    void stop();

    // Deduplicates on the full request identity, including activation, while a job is
    // still queued or running. Returns the existing id in that case.
    shared::Code submit(const JobSpec &spec, std::string *job_id);
    bool view(const std::string &job_id, JobView *out) const;
    shared::Code cancel(const std::string &job_id);
    // Newest job id, or empty when nothing was ever submitted.
    std::string latest_id() const;
    // Non-terminal job id, empty when idle.
    std::string running_id() const;

    // Startup: any non-terminal job file becomes failed/io_error.
    void recover();

private:
    void worker_loop();
    shared::Code execute(JobView *view, const JobSpec &spec);
    shared::Code resolve_archive(JobView *view, const JobSpec &spec, std::string *archive_path,
                                 std::string *display_name, std::string *expected_sha256);
    shared::Code import_archive_cached(const std::string &archive_path,
                                       const std::string &expected_sha256, std::string *sha256,
                                       std::vector<translation::ImportedTable> *tables,
                                       std::string *detail);
    shared::Code build_and_publish(JobView *view, const JobSpec &spec,
                                   const std::string &archive_sha256,
                                   const std::vector<translation::ImportedTable> &tables);
    void publish_view(const JobView &view);
    shared::Code persist(const JobView &view) const;
    bool load_job(const std::string &job_id, JobView *out) const;
    std::string dedupe_key(const JobSpec &spec) const;

    ServicePaths paths_;
    Logger *log_;
    DiagnosticsLog *diagnostics_;
    SourceRegistry *registry_;
    GenerationStore *generations_;

    mutable std::mutex mu_;
    std::condition_variable wake_;
    std::vector<JobView> jobs_;                 // newest last
    std::vector<std::pair<JobSpec, JobView>> pending_;
    std::map<std::string, std::string> dedupe_;  // key -> job id while active
    std::map<std::string, std::string> cancels_; // job id -> "1" requested
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    // Imported packs are expensive to unpack twice; keep the last few by digest.
    mutable std::mutex cache_mu_;
    std::vector<std::pair<std::string, std::vector<translation::ImportedTable>>> pack_cache_;
};

} // namespace bybus::service
