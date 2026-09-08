#include "proc.hpp"

#include <cerrno>
#include <csignal>
#include <stdexcept>

#ifndef __EMSCRIPTEN__
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "gil.hpp"

namespace proc {

#ifdef __EMSCRIPTEN__

ExecResult run(const std::string&, const std::vector<std::string>&) {
    throw std::runtime_error("jalankan_perintah(): proses eksternal tidak didukung di WASM browser");
}

StreamHandle startStream(const std::string&, const std::vector<std::string>&) {
    throw std::runtime_error("proc_stream_mulai(): proses eksternal tidak didukung di WASM browser");
}

std::string readStream(int, int) {
    return "";
}

void closeStream(pid_t, int) {}

#else

namespace {
void closeIfValid(int fd) {
    if (fd >= 0) close(fd);
}
}  // namespace

ExecResult run(const std::string& command, const std::vector<std::string>& args) {
    int outPipe[2];
    int errPipe[2];
    if (pipe(outPipe) != 0) throw std::runtime_error("jalankan_perintah(): gagal bikin pipe");
    if (pipe(errPipe) != 0) {
        closeIfValid(outPipe[0]);
        closeIfValid(outPipe[1]);
        throw std::runtime_error("jalankan_perintah(): gagal bikin pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        closeIfValid(outPipe[0]);
        closeIfValid(outPipe[1]);
        closeIfValid(errPipe[0]);
        closeIfValid(errPipe[1]);
        throw std::runtime_error("jalankan_perintah(): fork() gagal");
    }

    if (pid == 0) {
        // Child: redirect stdout/stderr into the pipes, then exec.
        // argv is built directly from `command`+`args` -- no shell
        // ever parses/interpolates this, so there's nothing for a
        // malicious argument to break out of.
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(command.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(command.c_str(), argv.data());
        _exit(127);  // only reached if execvp() failed (e.g. command not found)
    }

    // Parent.
    close(outPipe[1]);
    close(errPipe[1]);

    ExecResult result;
    {
        // A child process can run for a long time (a media download,
        // say) -- release the GIL so other goroutines keep running
        // while we wait, same as every other blocking call in net.cpp.
        GilRelease release;

        bool outOpen = true;
        bool errOpen = true;
        char buf[4096];
        while (outOpen || errOpen) {
            struct pollfd fds[2];
            int n = 0;
            int outIdx = -1;
            int errIdx = -1;
            if (outOpen) {
                fds[n].fd = outPipe[0];
                fds[n].events = POLLIN;
                outIdx = n;
                n++;
            }
            if (errOpen) {
                fds[n].fd = errPipe[0];
                fds[n].events = POLLIN;
                errIdx = n;
                n++;
            }
            int pr = poll(fds, static_cast<nfds_t>(n), -1);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (outIdx >= 0 && (fds[outIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t r = read(outPipe[0], buf, sizeof(buf));
                if (r > 0) {
                    result.out.append(buf, static_cast<size_t>(r));
                } else {
                    close(outPipe[0]);
                    outOpen = false;
                }
            }
            if (errIdx >= 0 && (fds[errIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t r = read(errPipe[0], buf, sizeof(buf));
                if (r > 0) {
                    result.err.append(buf, static_cast<size_t>(r));
                } else {
                    close(errPipe[0]);
                    errOpen = false;
                }
            }
        }

        int status = 0;
        waitpid(pid, &status, 0);
        result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return result;
}

StreamHandle startStream(const std::string& command, const std::vector<std::string>& args) {
    int outPipe[2];
    if (pipe(outPipe) != 0) return StreamHandle{};

    pid_t pid = fork();
    if (pid < 0) {
        closeIfValid(outPipe[0]);
        closeIfValid(outPipe[1]);
        return StreamHandle{};
    }

    if (pid == 0) {
        // Child: stdout -> pipe, stderr left alone (inherits this
        // process's own stderr -- shows up in the normal log, see
        // proc.hpp's comment on startStream()).
        dup2(outPipe[1], STDOUT_FILENO);
        close(outPipe[0]);
        close(outPipe[1]);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(command.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(command.c_str(), argv.data());
        _exit(127);  // only reached if execvp() failed
    }

    // Parent.
    close(outPipe[1]);
    StreamHandle h;
    h.pid = pid;
    h.fd = outPipe[0];
    return h;
}

std::string readStream(int fd, int maxLen) {
    if (fd < 0 || maxLen <= 0) return "";
    std::string buf(static_cast<size_t>(maxLen), '\0');
    ssize_t n;
    {
        GilRelease release;
        n = read(fd, buf.data(), buf.size());
    }
    if (n <= 0) return "";
    buf.resize(static_cast<size_t>(n));
    return buf;
}

void closeStream(pid_t pid, int fd) {
    if (pid > 0) {
        kill(pid, SIGKILL);
        int status = 0;
        // Release the GIL like every other wait -- never assume a wait
        // call "never blocks", even right after SIGKILL.
        GilRelease release;
        waitpid(pid, &status, 0);
    }
    closeIfValid(fd);
}

#endif

}  // namespace proc
