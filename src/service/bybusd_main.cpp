#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "service/commands.h"
#include "service/diagnostics.h"
#include "service/generation_store.h"
#include "service/import_session.h"
#include "service/ipc_server.h"
#include "service/job_queue.h"
#include "service/paths.h"
#include "service/source_registry.h"
#include "shared/fs.h"

namespace {

volatile sig_atomic_t g_terminate = 0;

void signal_handler(int) { g_terminate = 1; }

int fail(bybus::service::Logger *log, const char *stage) {
    if (log) log->error(stage);
    return 1;
}

} // namespace

int main() {
    using namespace bybus::service;
    ::umask(0077);
    ServicePaths paths;
    if (ensure_layout(paths) != bybus::shared::Code::Ok) return 1;

    // Keep the advisory lock open for the entire lifetime of the daemon.
    const int lock = ::open(paths.lock_file().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0) return 1;
    if (::flock(lock, LOCK_EX | LOCK_NB) != 0) {
        ::close(lock);
        return 0; // service.sh may safely attempt to launch a second instance
    }

    Logger log;
    if (log.open(paths.log_file()) != bybus::shared::Code::Ok) {
        ::close(lock);
        return 1;
    }
    DiagnosticsLog diagnostics;
    if (diagnostics.load(paths.diagnostics_file()) != bybus::shared::Code::Ok)
        return fail(&log, "diagnostics load failed");
    SourceRegistry registry(paths);
    if (registry.load() != bybus::shared::Code::Ok ||
        registry.ensure_default() != bybus::shared::Code::Ok)
        return fail(&log, "source registry failed");
    GenerationStore generations(paths);
    if (generations.cleanup_temps() != bybus::shared::Code::Ok)
        return fail(&log, "generation recovery failed");
    ImportSessions sessions(paths);
    if (sessions.cleanup_expired() != bybus::shared::Code::Ok)
        return fail(&log, "import recovery failed");
    JobQueue jobs(paths, &log, &diagnostics, &registry, &generations);
    jobs.recover();
    jobs.start();
    CommandService commands(paths, &log, &diagnostics, &registry, &generations,
                            &sessions, &jobs);
    IpcServer server(paths, &commands, &log);
    if (server.listen_socket() != bybus::shared::Code::Ok) {
        jobs.stop();
        return fail(&log, "socket listen failed");
    }
    if (write_atomic(paths.pid_file(), std::to_string(::getpid()) + "\n") !=
        bybus::shared::Code::Ok) {
        server.close_socket();
        jobs.stop();
        return fail(&log, "pid write failed");
    }

    ::signal(SIGTERM, signal_handler);
    ::signal(SIGINT, signal_handler);
    ::signal(SIGPIPE, SIG_IGN);
    std::atomic<bool> finished{false};
    std::thread monitor([&]() {
        while (!finished.load()) {
            if (g_terminate) { server.stop(); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    log.info("daemon ready");
    server.serve();
    finished.store(true);
    monitor.join();
    server.close_socket();
    jobs.stop();
    (void)::unlink(paths.pid_file().c_str());
    log.info("daemon stopped");
    ::close(lock);
    return 0;
}
