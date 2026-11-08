#pragma once

#include <sys/types.h>  // pid_t

#include <string>
#include <vector>

namespace proc {

struct ExecResult {
    int exitCode = -1;
    std::string out;
    std::string err;
};

// Runs `command` with `args` as a literal argv array -- never through a
// shell, so no shell-injection risk. Blocks (GIL released) until exit.
ExecResult run(const std::string& command, const std::vector<std::string>& args);

struct StreamHandle {
    pid_t pid = -1;  // -1 means startStream() failed (fork/pipe error)
    int fd = -1;     // child's stdout, readable end
};

// Like run(), but returns immediately with a handle for incremental
// reads (readStream()). Child's stderr stays connected to this
// process's own stderr, not captured -- use run() when that's needed.
StreamHandle startStream(const std::string& command, const std::vector<std::string>& args);

// One chunk (up to maxLen bytes). "" means EOF or error, same convention
// as tcpRecv(). GIL released while blocked in read().
std::string readStream(int fd, int maxLen);

// Kills the child if still running and reaps it, then closes `fd`. Safe
// to call after the child already exited on its own. Call exactly once
// per StreamHandle.
void closeStream(pid_t pid, int fd);

}  // namespace proc
