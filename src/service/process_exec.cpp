// service/process_exec.cpp
// Responsibility: API 24 compatible execution with a deadline, an output cap
// and per-line limits. Used by the install probe and the downloader only.
// Source: task contract §1 ("posix_spawn + 固定 argv"), §7, §9.
#include "service/process_exec.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>
#include <vector>

#include "service/paths.h"

namespace bybus::service {
namespace {

constexpr size_t kReadBufferBytes = 8192;

std::vector<char *> to_vector(std::vector<std::string> *storage) {
    std::vector<char *> pointers;
    pointers.reserve(storage->size() + 1);
    for (std::string &item : *storage) pointers.push_back(const_cast<char *>(item.c_str()));
    pointers.push_back(nullptr);
    return pointers;
}

int remaining_millis(int64_t deadline) {
    const int64_t left = deadline - monotonic_millis();
    if (left <= 0) return 0;
    if (left > 1000000) return 1000000;
    return static_cast<int>(left);
}

std::string limit_lines(const std::string &text, uint32_t max_lines, size_t max_line_bytes) {
    if (max_lines == 0 && max_line_bytes == 0) return text;
    std::string out;
    size_t start = 0;
    uint32_t count = 0;
    while (start <= text.size()) {
        const size_t newline = text.find('\n', start);
        const size_t end = newline == std::string::npos ? text.size() : newline;
        std::string line = text.substr(start, end - start);
        if (max_line_bytes > 0 && line.size() > max_line_bytes) line.resize(max_line_bytes);
        out += line;
        out.push_back('\n');
        ++count;
        if (newline == std::string::npos) break;
        if (max_lines > 0 && count >= max_lines) break;
        start = newline + 1;
    }
    return out;
}

} // namespace

bool is_executable_file(const std::string &path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return false;
    if (!S_ISREG(info.st_mode)) return false;
    return ::access(path.c_str(), X_OK) == 0;
}

ExecResult run(const std::string &program, const std::vector<std::string> &args,
               const ExecLimits &limits) {
    ExecResult result;
    if (program.empty() || program.front() != '/') return result;
    if (!is_executable_file(program)) return result;

    std::vector<std::string> argv_storage;
    argv_storage.reserve(args.size() + 1);
    argv_storage.push_back(program);
    for (const std::string &arg : args) argv_storage.push_back(arg);
    std::vector<char *> argv = to_vector(&argv_storage);

    std::vector<std::string> env_storage;
    env_storage.emplace_back("PATH=/system/bin:/system/xbin:/vendor/bin:/sbin");
    env_storage.emplace_back("ANDROID_DATA=/data");
    env_storage.emplace_back("ANDROID_ROOT=/system");
    env_storage.emplace_back("HOME=/");
    std::vector<char *> envp = to_vector(&env_storage);

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) return result;
    (void)::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)::fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    // Android's posix_spawn file actions are only exposed from API 28. Build
    // the argument vectors before fork; the child performs only async-signal-
    // safe syscalls before execve, so this also works in the threaded daemon.
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        const int null_in = ::open("/dev/null", O_RDONLY);
        const int null_err = ::open("/dev/null", O_WRONLY);
        if (null_in < 0 || null_err < 0 || ::dup2(null_in, STDIN_FILENO) < 0 ||
            ::dup2(fds[1], STDOUT_FILENO) < 0 || ::dup2(null_err, STDERR_FILENO) < 0) {
            ::_exit(127);
        }
        if (null_in > STDERR_FILENO) ::close(null_in);
        if (null_err > STDERR_FILENO) ::close(null_err);
        if (fds[1] > STDERR_FILENO) ::close(fds[1]);
        ::execve(program.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    if (pid < 0) {
        ::close(fds[0]);
        return result;
    }
    result.spawned = true;

    const int64_t deadline = monotonic_millis() + (limits.timeout_ms > 0 ? limits.timeout_ms : 1);
    bool finished = false;
    bool over_limit = false;
    while (!finished) {
        const int wait_ms = remaining_millis(deadline);
        if (wait_ms <= 0) {
            result.timed_out = true;
            break;
        }
        struct pollfd descriptor {};
        descriptor.fd = fds[0];
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) {
            result.timed_out = true;
            break;
        }
        char buffer[kReadBufferBytes];
        const ssize_t got = ::read(fds[0], buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) {
            finished = true;
            break;
        }
        const size_t chunk = static_cast<size_t>(got);
        if (result.output.size() + chunk > limits.max_output_bytes) {
            const size_t room = result.output.size() < limits.max_output_bytes
                                    ? static_cast<size_t>(limits.max_output_bytes) -
                                          result.output.size()
                                    : 0;
            result.output.append(buffer, room);
            result.truncated = true;
            over_limit = true;
            break;
        }
        result.output.append(buffer, chunk);
    }
    ::close(fds[0]);
    if (!finished || result.timed_out || over_limit) (void)::kill(pid, SIGKILL);

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == pid && WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);

    if (limits.max_lines > 0 || limits.max_line_bytes > 0) {
        result.output = limit_lines(result.output, limits.max_lines, limits.max_line_bytes);
    }
    return result;
}

} // namespace bybus::service
