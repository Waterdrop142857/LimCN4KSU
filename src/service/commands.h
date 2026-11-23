// service/commands.h
// Responsibility: turn one parsed protocol request into one response object.
// Every handler re-validates its arguments, calls the owning layer, and emits
// the exact data shape from 20-protocol.md 搂3. No handler touches the socket.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "service/diagnostics.h"
#include "service/game_scan.h"
#include "service/generation_store.h"
#include "service/import_session.h"
#include "service/job_queue.h"
#include "service/paths.h"
#include "service/source_registry.h"
#include "shared/json.h"
#include "shared/status.h"

namespace bybus::service {

struct Request {
    std::string command;       // words joined by a single space
    shared::JsonPtr args;      // object, never null
};

struct Response {
    shared::Code code = shared::Code::Ok;
    std::string message;
    shared::JsonPtr data;
};

// Parses `{"schema":1,"command":"...","args":{...}}`. Returns false with a code
// when the envelope itself is invalid.
bool parse_request(const std::string &line, Request *out, shared::Code *code);

class CommandService {
public:
    CommandService(const ServicePaths &paths, Logger *log, DiagnosticsLog *diagnostics,
                   SourceRegistry *registry, GenerationStore *generations,
                   ImportSessions *sessions, JobQueue *jobs);
    ~CommandService();
    CommandService(const CommandService &) = delete;
    CommandService &operator=(const CommandService &) = delete;

    Response handle(const Request &request);
    std::string render(const Response &response) const;
    bool shutdown_requested() const { return shutdown_requested_.load(); }

private:
    Response status();
    Response sources_list();
    Response sources_select(const Request &request);
    Response sources_remove(const Request &request);
    Response update_start(const Request &request);
    Response import_begin(const Request &request);
    Response import_chunk(const Request &request);
    Response import_commit(const Request &request);
    Response import_abort(const Request &request);
    Response snapshot_build(const Request &request);
    Response job_status(const Request &request);
    Response job_cancel(const Request &request);
    Response diagnostics_view();
    Response uninstall_cleanup(const Request &request);
    Response shutdown();
    Response runtime_probe();
    // "ready" when the active manifest carries a verified AppSealing profile,
    // "invalid" when the manifest is unreadable, "missing" otherwise.
    std::string profile_state() const;
    std::string profile_code() const;

    ServicePaths paths_;
    Logger *log_;
    DiagnosticsLog *diagnostics_;
    SourceRegistry *registry_;
    GenerationStore *generations_;
    ImportSessions *sessions_;
    JobQueue *jobs_;
    GameProbe probe_;
    std::atomic<bool> shutdown_requested_{false};
};

} // namespace bybus::service

