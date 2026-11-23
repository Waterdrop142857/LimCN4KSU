#include "service/commands.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "service/file_ops.h"
#include "service/receipt.h"
#include "service/source_fetch.h"
#include "shared/fs.h"
#include "shared/sha256.h"
#include "translation/snapshot.h"

#include <cstdlib>

namespace bybus::service {
namespace {

shared::JsonPtr object() { return shared::Json::object(); }

bool string_arg(const shared::JsonPtr &args, const char *name, std::string *out,
                bool required = true) {
    const shared::JsonPtr value = args ? args->opt(name) : nullptr;
    if (!value || !value->is_string()) return !required;
    *out = value->as_string();
    return true;
}

bool int_arg(const shared::JsonPtr &args, const char *name, int64_t *out, bool required = true) {
    const shared::JsonPtr value = args ? args->opt(name) : nullptr;
    if (!value || !value->is_integral()) return !required;
    if (value->as_int() < 0) return false;
    *out = value->as_int();
    return true;
}

bool bool_arg(const shared::JsonPtr &args, const char *name, bool fallback) {
    const shared::JsonPtr value = args ? args->opt(name) : nullptr;
    if (!value || !value->is_bool()) return fallback;
    return value->as_bool();
}

// The daemon reads the chosen file as root. Archive type is checked from its
// bytes by the importer, so a filename extension is not required.
bool valid_source_path(const std::string &path) {
    return valid_absolute_path(path);
}

std::string read_prop(const std::string &path, const std::string &key) {
    std::string text;
    if (shared::read_file(path, 64 * 1024, &text) != shared::Code::Ok) return std::string();
    const std::string needle = key + "=";
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        const std::string line = text.substr(at, end == std::string::npos ? std::string::npos
                                                                         : end - at);
        if (line.compare(0, needle.size(), needle) == 0) {
            std::string value = line.substr(needle.size());
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
            return value;
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return std::string();
}

uint64_t directory_bytes(const std::string &root) {
    std::vector<std::string> pending{root};
    uint64_t total = 0;
    while (!pending.empty()) {
        const std::string dir_path = std::move(pending.back());
        pending.pop_back();
        DIR *dir = ::opendir(dir_path.c_str());
        if (!dir) continue;
        while (dirent *entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            const std::string path = dir_path + "/" + name;
            struct stat info {};
            if (::lstat(path.c_str(), &info) != 0) continue;
            if (S_ISDIR(info.st_mode)) {
                pending.push_back(path);
            } else if (S_ISREG(info.st_mode) && info.st_size > 0) {
                const uint64_t size = static_cast<uint64_t>(info.st_size);
                const uint64_t ceiling = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
                total = size > ceiling - total ? ceiling : total + size;
            }
        }
        ::closedir(dir);
    }
    return total;
}

Response failure(shared::Code code) {
    Response response;
    response.code = code;
    response.message = shared::code_message(code);
    response.data = object();
    return response;
}

Response success(shared::JsonPtr data) {
    Response response;
    response.code = shared::Code::Ok;
    response.data = data ? std::move(data) : object();
    return response;
}

} // namespace

bool parse_request(const std::string &line, Request *out, shared::Code *code) {
    const shared::ParseOutcome parsed = shared::parse(line);
    if (!parsed.ok || !parsed.value || !parsed.value->is_object()) {
        *code = shared::Code::InvalidArgument;
        return false;
    }
    const shared::JsonPtr schema = parsed.value->opt("schema");
    if (!schema || !schema->is_integral() || schema->as_int() != 1) {
        *code = shared::Code::InvalidArgument;
        return false;
    }
    const shared::JsonPtr command = parsed.value->opt("command");
    if (!command || !command->is_string() || command->as_string().empty() ||
        command->as_string().size() > 64) {
        *code = shared::Code::InvalidArgument;
        return false;
    }
    std::string words;
    for (const char c : command->as_string()) {
        const bool allowed = (c >= 'a' && c <= 'z') || c == ' ' || c == '_';
        if (!allowed) {
            *code = shared::Code::UnknownCommand;
            return false;
        }
        words.push_back(c == '_' ? ' ' : c);
    }
    const shared::JsonPtr args = parsed.value->opt("args");
    if (args && !args->is_object()) {
        *code = shared::Code::InvalidArgument;
        return false;
    }
    out->command = words;
    out->args = args ? args : object();
    *code = shared::Code::Ok;
    return true;
}

CommandService::CommandService(const ServicePaths &paths, Logger *log,
                               DiagnosticsLog *diagnostics, SourceRegistry *registry,
                               GenerationStore *generations, ImportSessions *sessions,
                               JobQueue *jobs)
    : paths_(paths), log_(log), diagnostics_(diagnostics), registry_(registry),
      generations_(generations), sessions_(sessions), jobs_(jobs) {
    if (log_) log_->info("command service initialized");
}

CommandService::~CommandService() = default;

std::string CommandService::render(const Response &response) const {
    shared::JsonPtr root = object();
    root->put_int("schema", 1);
    root->put_bool("ok", response.code == shared::Code::Ok);
    root->put_string("code", shared::code_text(response.code));
    root->put_string("message", response.code == shared::Code::Ok ? std::string()
                                                                 : shared::code_message(response.code));
    root->put("data", response.data ? response.data : object());
    return root->dump();
}

Response CommandService::handle(const Request &request) {
    const std::string &command = request.command;
    if (command == "status") return status();
    if (command == "sources list") return sources_list();
    if (command == "sources select") return sources_select(request);
    if (command == "sources remove") return sources_remove(request);
    if (command == "update start") return update_start(request);
    if (command == "import begin") return import_begin(request);
    if (command == "import chunk") return import_chunk(request);
    if (command == "import commit") return import_commit(request);
    if (command == "import abort") return import_abort(request);
    if (command == "snapshot build") return snapshot_build(request);
    if (command == "job status") return job_status(request);
    if (command == "job cancel") return job_cancel(request);
    if (command == "diagnostics") return diagnostics_view();
    if (command == "runtime probe") return runtime_probe();
    if (command == "uninstall cleanup") return uninstall_cleanup(request);
    if (command == "shutdown") return shutdown();
    return failure(shared::Code::UnknownCommand);
}

Response CommandService::status() {
    shared::JsonPtr data = object();
    shared::JsonPtr module = data->put_object("module");
    module->put_string("id", read_prop(paths_.module_prop(), "id"));
    module->put_string("version", read_prop(paths_.module_prop(), "version"));
    const std::string version_code = read_prop(paths_.module_prop(), "versionCode");
    module->put_int("versionCode", version_code.empty() ? 0 : std::atoll(version_code.c_str()));
    module->put_string("state", shared::exists(paths_.module_root + "/disable") ? "disabled"
                                                                               : "enabled");

    shared::JsonPtr zygisk = data->put_object("zygisk");
    const std::string zygisk_prop = "/data/adb/modules/zygisksu/module.prop";
    const bool zygisk_present =
        shared::exists(zygisk_prop) && !shared::exists("/data/adb/modules/zygisksu/disable");
    zygisk->put_bool("available", zygisk_present);
    zygisk->put_string("provider", zygisk_present ? "zygisknext" : "none");
    zygisk->put_string("code", zygisk_present ? "ok" : "zygisk_missing");

    GameInstall install;
    GameTextScan texts;
    probe_.snapshot(false, &install, &texts);
    shared::JsonPtr game = data->put_object("game");
    game->put_bool("installed", install.installed);
    game->put_string("packageName", install.package_name);
    game->put_int("versionCode", install.version_code);
    game->put_string("versionName", install.version_name);
    game->put_string("installIdentity", install.install_identity);
    const char *text_state = "waiting_for_game_text";
    const char *text_code = "game_missing_text";
    if (!install.installed) {
        text_state = "game_missing";
        text_code = "game_missing";
    } else if (texts.code == shared::Code::Ok) {
        text_state = "ready";
        text_code = "ok";
    }
    game->put_string("textState", text_state);
    game->put_string("textCode", text_code);
    shared::JsonPtr locales = game->put_array("locales");
    for (const std::string &locale : texts.locales) locales->append(shared::Json::string(locale));
    game->put_int("tableCount", static_cast<int64_t>(texts.files.size()));
    game->put_string("profileState", profile_state());
    game->put_string("profileCode", profile_code());

    const ActiveSnapshot active = generations_->inspect_active();
    shared::JsonPtr snapshot = data->put_object("snapshot");
    const char *snapshot_state = "missing";
    if (active.present) snapshot_state = active.valid ? "ready" : "invalid";
    snapshot->put_string("state", snapshot_state);
    snapshot->put_string("generation", active.generation);
    snapshot->put_string("sourceId", active.source_id);
    snapshot->put_int("builtAtMillis", active.built_at_millis);
    snapshot->put_int("tableCount", static_cast<int64_t>(active.table_count));
    snapshot->put_int("runtimeSchema", 1);
    snapshot->put_string("code", shared::code_text(active.code));

    const SourceRecord selected = registry_->selected();
    shared::JsonPtr source = data->put_object("source");
    source->put_string("id", selected.id);
    source->put_string("name", selected.name);
    source->put_string("kind", selected.kind.empty() ? "official" : selected.kind);
    source->put_string("revision", selected.revision);
    source->put_int("tableCount", static_cast<int64_t>(selected.table_count));
    source->put_bool("pendingActivation",
                     active.present && !active.source_id.empty() && active.source_id != selected.id);

    shared::JsonPtr job = data->put_object("job");
    JobView view;
    const std::string running = jobs_->running_id();
    const std::string job_id = running.empty() ? jobs_->latest_id() : running;
    if (!job_id.empty() && jobs_->view(job_id, &view)) {
        job->put_string("id", view.id);
        job->put_string("state", job_state_text(view.state));
        job->put_string("stage", view.stage);
        job->put_int("done", view.done);
        job->put_int("total", view.total);
        job->put_string("code", view.error_code.empty() ? "ok" : view.error_code);
        job->put_string("detail", view.detail);
    } else {
        job->put_string("id", "");
        job->put_string("state", "idle");
        job->put_string("stage", "");
        job->put_int("done", 0);
        job->put_int("total", 0);
        job->put_string("code", "ok");
    }

    shared::JsonPtr storage = data->put_object("storage");
    const uint64_t archives = directory_bytes(paths_.archives());
    const uint64_t generations = directory_bytes(paths_.generations());
    const uint64_t ceiling = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    const uint64_t used = generations > ceiling - archives ? ceiling : archives + generations;
    storage->put_int("usedBytes", static_cast<int64_t>(used));
    storage->put_int("limitBytes", static_cast<int64_t>(kStorageLimitBytes));

    data->put_string("code", "ok");
    return success(data);
}

Response CommandService::runtime_probe() { return status(); }

Response CommandService::sources_list() {
    shared::JsonPtr data = object();
    shared::JsonPtr rows = data->put_array("sources");
    for (const SourceRecord &record : registry_->list()) {
        shared::JsonPtr row = object();
        row->put_string("id", record.id);
        row->put_string("name", record.name);
        row->put_string("kind", record.kind);
        row->put_string("revision", record.revision);
        row->put_int("tableCount", static_cast<int64_t>(record.table_count));
        row->put_string("archiveSha256", record.archive_sha256);
        row->put_int("updatedAtMillis", record.updated_at_millis);
        rows->append(row);
    }
    data->put_string("selectedId", registry_->selected_id());
    return success(data);
}

Response CommandService::sources_select(const Request &request) {
    std::string id;
    if (!string_arg(request.args, "id", &id) || !valid_source_id(id)) {
        return failure(shared::Code::InvalidArgument);
    }
    SourceRecord record;
    if (!registry_->find(id, &record)) return failure(shared::Code::SourceMissing);
    const shared::Code code = registry_->select(id);
    if (code != shared::Code::Ok) return failure(code);
    shared::JsonPtr data = object();
    data->put_string("selectedId", id);
    data->put_string("effective", "next_launch");
    return success(data);
}

Response CommandService::sources_remove(const Request &request) {
    std::string id;
    if (!string_arg(request.args, "id", &id) || !valid_source_id(id)) {
        return failure(shared::Code::InvalidArgument);
    }
    if (id == kDefaultSourceId) return failure(shared::Code::InvalidArgument);
    std::vector<SourceRecord> remaining;
    const shared::Code code = registry_->remove(id, &remaining);
    if (code != shared::Code::Ok) return failure(code);
    shared::JsonPtr data = object();
    shared::JsonPtr rows = data->put_array("sources");
    for (const SourceRecord &record : remaining) {
        shared::JsonPtr row = object();
        row->put_string("id", record.id);
        row->put_string("name", record.name);
        row->put_string("kind", record.kind);
        row->put_string("revision", record.revision);
        row->put_int("tableCount", static_cast<int64_t>(record.table_count));
        rows->append(row);
    }
    data->put_string("selectedId", registry_->selected_id());
    return success(data);
}

Response CommandService::update_start(const Request &request) {
    std::string mode;
    if (!string_arg(request.args, "mode", &mode)) return failure(shared::Code::InvalidArgument);
    JobSpec spec;
    spec.kind = "update";
    spec.mode = mode;
    spec.activate = true;
    if (mode == "release") {
        spec.source_id = kDefaultSourceId;
        const shared::JsonPtr asset_url = request.args ? request.args->opt("assetUrl") : nullptr;
        if (asset_url) {
            if (!asset_url->is_string() || !valid_release_asset_url(asset_url->as_string())) {
                return failure(shared::Code::InvalidArgument);
            }
            spec.url = asset_url->as_string();
            const shared::JsonPtr digest = request.args->opt("expectedSha256");
            if (digest) {
                if (!digest->is_string() || !shared::is_sha256_hex(digest->as_string())) {
                    return failure(shared::Code::InvalidArgument);
                }
                spec.expected_sha256 = digest->as_string();
            }
        } else if (request.args && request.args->has("expectedSha256")) {
            return failure(shared::Code::InvalidArgument);
        }
    }
    if (mode == "path") {
        std::string path;
        if (!string_arg(request.args, "path", &path) || !valid_source_path(path)) {
            return failure(shared::Code::InvalidArgument);
        }
        spec.local_path = path;
    } else if (mode == "url") {
        std::string url;
        if (!string_arg(request.args, "url", &url) || !valid_download_url(url)) {
            return failure(shared::Code::InvalidArgument);
        }
        spec.url = url;
    } else if (mode != "release" && mode != "inbox") {
        return failure(shared::Code::InvalidArgument);
    }
    std::string job_id;
    const shared::Code code = jobs_->submit(spec, &job_id);
    if (code != shared::Code::Ok) return failure(code);
    shared::JsonPtr data = object();
    data->put_string("jobId", job_id);
    data->put_string("state", "queued");
    return success(data);
}

Response CommandService::import_begin(const Request &request) {
    std::string name;
    int64_t total = 0;
    std::string sha256;
    if (!string_arg(request.args, "name", &name) || !int_arg(request.args, "totalBytes", &total) ||
        total <= 0 || static_cast<uint64_t>(total) > kMaxArchiveBytes ||
        name.size() > kMaxImportNameBytes) {
        return failure(shared::Code::InvalidArgument);
    }
    string_arg(request.args, "sha256", &sha256, false);
    if (!sha256.empty() && !shared::is_sha256_hex(sha256)) {
        return failure(shared::Code::InvalidArgument);
    }
    std::string session;
    const shared::Code code =
        sessions_->begin(name, static_cast<uint64_t>(total), sha256, &session);
    if (code != shared::Code::Ok) return failure(code);
    shared::JsonPtr data = object();
    data->put_string("sessionId", session);
    data->put_int("chunkBytes", static_cast<int64_t>(kImportChunkBytes));
    return success(data);
}

Response CommandService::import_chunk(const Request &request) {
    std::string session;
    std::string base64;
    int64_t index = 0;
    if (!string_arg(request.args, "sessionId", &session) || !valid_session_id(session) ||
        !string_arg(request.args, "base64", &base64) || base64.size() > kImportChunkBytes * 2 ||
        !int_arg(request.args, "index", &index)) {
        return failure(shared::Code::InvalidArgument);
    }
    uint64_t received = 0;
    uint64_t total = 0;
    const shared::Code code =
        sessions_->chunk(session, static_cast<uint64_t>(index), base64, &received, &total);
    if (code != shared::Code::Ok) return failure(code);
    shared::JsonPtr data = object();
    data->put_int("received", static_cast<int64_t>(received));
    data->put_int("total", static_cast<int64_t>(total));
    return success(data);
}

Response CommandService::import_commit(const Request &request) {
    std::string session;
    std::string name;
    if (!string_arg(request.args, "sessionId", &session) || !valid_session_id(session) ||
        !string_arg(request.args, "name", &name) || name.empty() || name.size() > kMaxImportNameBytes) {
        return failure(shared::Code::InvalidArgument);
    }
    const bool activate = bool_arg(request.args, "activate", true);
    const std::string archive = paths_.import_dir(session) + "/archive.pack";
    std::string sha256;
    uint64_t bytes = 0;
    shared::Code code = sessions_->assemble(session, archive, &sha256, &bytes);
    if (code != shared::Code::Ok) return failure(code);
    if (hash_file(archive, &sha256, &bytes) != shared::Code::Ok) {
        return failure(shared::Code::IoError);
    }
    // The background job must outlive the upload session. Publish a verified
    // copy under archives before returning its job ID to the WebUI.
    const std::string durable = paths_.archive_file(sha256);
    std::string stored_sha256;
    const bool archive_ready = shared::exists(durable) &&
        hash_file(durable, &stored_sha256, nullptr) == shared::Code::Ok &&
        stored_sha256 == sha256;
    if (!archive_ready) {
        const std::string temporary = durable + ".tmp-" + session;
        std::string copied_sha256;
        uint64_t copied_bytes = 0;
        const shared::Code copied = copy_file_hashed(archive, temporary, kMaxArchiveBytes,
                                                     &copied_sha256, &copied_bytes);
        if (copied != shared::Code::Ok || copied_sha256 != sha256 || copied_bytes != bytes) {
            (void)shared::remove_tree(temporary);
            return failure(copied == shared::Code::Ok ? shared::Code::ArchiveInvalid : copied);
        }
        const shared::Code moved = shared::atomic_replace(temporary, durable);
        if (moved != shared::Code::Ok) return failure(moved);
    }
    JobSpec spec;
    spec.kind = "update";
    spec.mode = "import";
    spec.archive_path = durable;
    spec.display_name = name;
    spec.expected_sha256 = sha256;
    spec.source_id = std::string(kLocalSourcePrefix) + sha256.substr(0, 16);
    spec.activate = activate;
    std::string job_id;
    code = jobs_->submit(spec, &job_id);
    if (code != shared::Code::Ok) return failure(code);
    (void)sessions_->abort(session);
    shared::JsonPtr data = object();
    data->put_string("jobId", job_id);
    data->put_string("sourceId", spec.source_id);
    return success(data);
}

Response CommandService::import_abort(const Request &request) {
    std::string session;
    if (!string_arg(request.args, "sessionId", &session) || !valid_session_id(session)) {
        return failure(shared::Code::InvalidArgument);
    }
    const shared::Code code = sessions_->abort(session);
    if (code != shared::Code::Ok) return failure(code);
    return success(object());
}

Response CommandService::snapshot_build(const Request &request) {
    std::string id;
    string_arg(request.args, "sourceId", &id, false);
    if (id.empty()) id = registry_->selected_id();
    if (!valid_source_id(id)) return failure(shared::Code::InvalidArgument);
    SourceRecord record;
    if (!registry_->find(id, &record)) return failure(shared::Code::SourceMissing);
    if (id == kDefaultSourceId && record.archive_sha256.empty()) {
        // Older states may only record the default archive in the active snapshot.
        const ActiveSnapshot active = generations_->inspect_active();
        if (active.archive_sha256.empty()) return failure(shared::Code::NetworkUnavailable);
        record.archive_sha256 = active.archive_sha256;
    }
    const std::string archive = paths_.archive_file(record.archive_sha256);
    if (!shared::is_sha256_hex(record.archive_sha256) || !shared::exists(archive)) {
        return failure(shared::Code::SourceMissing);
    }
    JobSpec spec;
    spec.kind = "build";
    spec.mode = "source";
    spec.archive_path = archive;
    spec.source_id = id;
    spec.display_name = record.name;
    spec.activate = true;
    spec.expected_sha256 = record.archive_sha256;
    std::string job_id;
    const shared::Code code = jobs_->submit(spec, &job_id);
    if (code != shared::Code::Ok) return failure(code);
    shared::JsonPtr data = object();
    data->put_string("jobId", job_id);
    data->put_string("state", "queued");
    return success(data);
}

Response CommandService::job_status(const Request &request) {
    std::string id;
    if (!string_arg(request.args, "id", &id) || !valid_job_id(id)) {
        return failure(shared::Code::InvalidArgument);
    }
    JobView view;
    if (!jobs_->view(id, &view)) return failure(shared::Code::JobMissing);
    shared::JsonPtr data = object();
    data->put_string("id", view.id);
    data->put_string("state", job_state_text(view.state));
    data->put_string("stage", view.stage);
    data->put_int("done", view.done);
    data->put_int("total", view.total);
    data->put_string("code", terminal(view.state) && !view.error_code.empty() ? view.error_code
                                                                             : "ok");
    data->put_string("errorCode", view.error_code);
    data->put_string("detail", view.detail);
    data->put_int("startedAtMillis", view.started_at_millis);
    shared::JsonPtr progress = data->put_object("progress");
    progress->put_string("phase", view.stage);
    progress->put_int("bytesDone", view.done);
    progress->put_int("bytesTotal", view.total);
    return success(data);
}

Response CommandService::job_cancel(const Request &request) {
    std::string id;
    if (!string_arg(request.args, "id", &id) || !valid_job_id(id)) {
        return failure(shared::Code::InvalidArgument);
    }
    const shared::Code code = jobs_->cancel(id);
    if (code == shared::Code::JobMissing) return failure(code);
    if (code != shared::Code::Ok) return failure(code);
    shared::JsonPtr data = object();
    data->put_string("id", id);
    data->put_string("state", "cancelled");
    return success(data);
}

Response CommandService::diagnostics_view() {
    shared::JsonPtr data = object();
    const RuntimeReceipt receipt = read_receipt(paths_.receipt_file());
    shared::JsonPtr runtime = data->put_object("runtime");
    runtime->put_string("generation", receipt.generation);
    runtime->put_int("hits", static_cast<int64_t>(receipt.hits));
    runtime->put_int("misses", static_cast<int64_t>(receipt.misses));
    runtime->put_string("fontState", receipt.font_state.empty() ? "unknown" : receipt.font_state);
    runtime->put_string("hookState", receipt.hook_state.empty() ? "unknown" : receipt.hook_state);
    runtime->put_string("compatState",
                        receipt.compat_state.empty() ? "unknown" : receipt.compat_state);
    runtime->put_string("code", receipt.code.empty() ? "ok" : receipt.code);

    const SourceRegistry::BuildStats stats = registry_->build_stats();
    shared::JsonPtr build = data->put_object("build");
    build->put_string("lastGeneration", stats.last_generation);
    build->put_string("lastCode", stats.last_code);
    build->put_int("generations", stats.generations);
    build->put_int("discarded", stats.discarded);
    build->put_string("profileState", profile_state());
    build->put_string("profileCode", profile_code());

    shared::JsonPtr log = data->put_array("log");
    if (diagnostics_) {
        const std::vector<DiagnosticEntry> entries = diagnostics_->entries();
        for (size_t i = 0; i < entries.size() && i < kMaxDiagnosticsEntries; ++i) {
            const DiagnosticEntry &entry = entries[i];
            shared::JsonPtr row = object();
            row->put_int("atMillis", entry.at_millis);
            row->put_string("stage", entry.stage);
            row->put_string("code", entry.code);
            row->put_int("count", static_cast<int64_t>(entry.count));
            log->append(row);
        }
    }
    return success(data);
}

std::string CommandService::profile_state() const {
    const ActiveSnapshot active = generations_->inspect_active();
    if (!active.present) return "missing";
    std::string text;
    if (shared::read_file(paths_.generation_dir(active.generation) + "/" +
                              translation::kManifestName,
                          kMaxManifestBytes, &text) != shared::Code::Ok) {
        return "invalid";
    }
    translation::SnapshotManifest manifest;
    std::string error;
    if (!translation::parse_manifest(text, &manifest, &error)) return "invalid";
    if (!manifest.profile.present) return "missing";
    if (!manifest.profile.protection_sha256.empty() &&
        !manifest.profile.appsealing.present) {
        return "unsupported";
    }
    return "ready";
}

std::string CommandService::profile_code() const {
    const std::string state = profile_state();
    if (state == "ready") return "ok";
    if (state == "invalid") return "snapshot_invalid";
    return "unsupported_runtime";
}

Response CommandService::uninstall_cleanup(const Request &request) {
    const shared::JsonPtr confirm = request.args ? request.args->opt("confirm") : nullptr;
    if (!confirm || !confirm->is_bool() || !confirm->as_bool()) {
        return failure(shared::Code::InvalidArgument);
    }
    // Archives, sources and generations are user data and stay in place.
    shared::JsonPtr data = object();
    shared::JsonPtr removed = data->put_array("removed");
    const std::string targets[] = {paths_.imports(), paths_.jobs(), paths_.runtime()};
    for (const std::string &target : targets) {
        if (!shared::is_directory(target)) continue;
        std::vector<std::string> names;
        if (shared::list_dir(target, 512, &names) != shared::Code::Ok) continue;
        for (const std::string &name : names) {
            if (shared::remove_tree(target + "/" + name) == shared::Code::Ok) {
                removed->append(shared::Json::string(name));
            }
        }
    }
    return success(data);
}

Response CommandService::shutdown() {
    shutdown_requested_ = true;
    shared::JsonPtr data = object();
    data->put_string("state", "shutting_down");
    return success(data);
}

} // namespace bybus::service

