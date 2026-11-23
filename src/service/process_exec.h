// service/process_exec.h
// Responsibility: the only way this daemon starts another program.
// Structured spawn with an absolute path, a fixed argv vector, stdout
// captured into a bounded buffer, stdin/stderr on /dev/null, an overall
// deadline and a hard output cap. No shell, no PATH lookup, no popen/system.
// Source: task contract §1, §9; 10-port-plan.md §3.2.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bybus::service {

struct ExecLimits {
    uint64_t max_output_bytes = 1024ull * 1024;
    int timeout_ms = 5000;
    // 0 keeps every line; otherwise only the first `max_lines` lines are kept
    // and each line is cut at `max_line_bytes` (0 == unlimited).
    uint32_t max_lines = 0;
    size_t max_line_bytes = 0;
};

struct ExecResult {
    bool spawned = false;
    bool timed_out = false;
    bool truncated = false;
    int exit_code = -1;  // WEXITSTATUS; -1 when the child was signalled or not reaped
    std::string output;
};

// `program` must be an absolute path to an executable file. `args` excludes
// argv[0] and is passed verbatim.
ExecResult run(const std::string &program, const std::vector<std::string> &args,
               const ExecLimits &limits);

// Regular file with an execute bit set.
bool is_executable_file(const std::string &path);

} // namespace bybus::service
