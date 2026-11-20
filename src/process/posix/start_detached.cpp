#include "process/detached.h"

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

#include "base/utf8.h"

namespace fv {

namespace {

// In the child, before exec: closes every descriptor above stderr except `keep`, as
// subprocess.Popen's close_fds=True does, so the new process holds none of this one's files,
// locks or sockets (a restarted app would otherwise inherit its predecessor's data lock).
// Async-signal-safe: nothing is allocated between fork and exec.
void close_other_descriptors(int keep) {
#ifdef SYS_close_range
    if (keep >= 3) {
        const bool below =
            keep == 3 || ::syscall(SYS_close_range, 3U, static_cast<unsigned>(keep) - 1U, 0U) == 0;
        if (below && ::syscall(SYS_close_range, static_cast<unsigned>(keep) + 1U, ~0U, 0U) == 0) {
            return;
        }
    }
#endif
    long limit = ::sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > 65536) {
        limit = 65536;
    }
    for (int fd = 3; fd < static_cast<int>(limit); ++fd) {
        if (fd != keep) {
            ::close(fd);
        }
    }
}

// One report from a child. Written with a single write(2) of a few bytes, so it is never split
// or interleaved with the other child's.
struct Message {
    enum Kind : int { started = 1, failed = 2 };
    int kind;
    int value; // the program's process id, or the errno of what failed
};

// Async-signal-safe.
void write_message(int fd, const Message& message) {
    ssize_t written = 0;
    do {
        written = ::write(fd, &message, sizeof(message));
    } while (written < 0 && errno == EINTR);
}

} // namespace

std::expected<std::uint32_t, std::string> start_detached(const std::filesystem::path& program,
                                                         std::span<const std::string> args,
                                                         const std::filesystem::path& cwd) {
    const std::string failure = "Could not start " + path_to_utf8(program.filename()) + ".";
    const std::string path = path_to_utf8(program);
    const std::string directory = path_to_utf8(cwd.empty() ? program.parent_path() : cwd);
    std::vector<std::string> owned;
    owned.reserve(args.size() + 1);
    owned.push_back(path);
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& arg : owned) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    // A double fork: the intermediate child starts the program and exits at once, so this
    // process reaps it now and the program, orphaned, is reaped by init when it ends rather than
    // staying a zombie of ours until we exit (subprocess.Popen reaped its child later).
    // Both children report through this pipe in whole messages (atomic, being under PIPE_BUF),
    // because the program's failure can arrive before the intermediate's pid: the program's id,
    // a failed chdir or exec, a failed second fork. A clean exec closes the pipe's last end.
    int report[2];
    if (::pipe2(report, O_CLOEXEC) != 0) {
        return std::unexpected(failure);
    }
    const pid_t middle = ::fork();
    if (middle < 0) {
        ::close(report[0]);
        ::close(report[1]);
        return std::unexpected(failure);
    }
    if (middle == 0) {
        ::close(report[0]);
        // A new session, led by this intermediate; the program is not its leader, so it can never
        // take a controlling terminal.
        ::setsid();
        const pid_t pid = ::fork();
        if (pid != 0) {
            const Message message{pid > 0 ? Message::started : Message::failed,
                                  pid > 0 ? static_cast<int>(pid) : errno};
            write_message(report[1], message);
            ::_exit(0);
        }
        close_other_descriptors(report[1]);
        int error = 0;
        if (!directory.empty() && ::chdir(directory.c_str()) != 0) {
            error = errno;
        } else {
            ::execv(path.c_str(), argv.data());
            error = errno;
        }
        write_message(report[1], Message{Message::failed, error});
        ::_exit(127);
    }
    ::close(report[1]);
    pid_t started = 0;
    bool failed = false;
    for (;;) {
        Message message{};
        ssize_t got = 0;
        do {
            got = ::read(report[0], &message, sizeof(message));
        } while (got < 0 && errno == EINTR);
        if (got != static_cast<ssize_t>(sizeof(message))) {
            break;
        }
        if (message.kind == Message::started) {
            started = static_cast<pid_t>(message.value);
        } else {
            failed = true;
        }
    }
    ::close(report[0]);
    int status = 0;
    while (::waitpid(middle, &status, 0) < 0 && errno == EINTR) {
    }
    if (failed || started <= 0) {
        return std::unexpected(failure);
    }
    return static_cast<std::uint32_t>(started);
}

} // namespace fv
